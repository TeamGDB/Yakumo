#include "mods/mhp3rd_mods.hpp"

#include "mods/mhp3rd_added_files.hpp"
#include "mods/file_overlay.hpp"
#include "mods/mhp3rd_code_mods.hpp"
#include "mods/mhp3rd_data_bin.hpp"
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mhp3rd_quests.hpp"
#include "mods/mhp3rd_tables.hpp"
#include "mods/mod_ini.hpp"
#include "mods/mod_native.hpp"
#include "mods/mod_script.hpp"

#include "install/user_data.hpp"
#include "kernel/iso_image.hpp"
#include "kernel/kernel.hpp"
#include "mhp3rd_profile.hpp"
#include "overlays.hpp"

#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <vector>

namespace mhp3rd::mods {
namespace {

namespace fs = std::filesystem;
using p3rd::ArchiveView;
using p3rd::Directory;
using p3rd::Layout;

constexpr const char *kDataBin = "PSP_GAME/USRDIR/DATA.BIN";
// Yakumo's own stubs at the bottom of user memory (kernel.hpp): mods must not overwrite them.
constexpr std::uint32_t kHostStubsEnd = kGuestCallReturnStub + 4u;

bool trace() {
    static const bool enabled = std::getenv("MHP3RD_TRACE_MODS") != nullptr;
    return enabled;
}

const char *disabled_by() {
    const char *value = std::getenv("MHP3RD_NO_MODS");
    return value != nullptr && *value != '\0' && std::string(value) != "0" ? "MHP3RD_NO_MODS" : nullptr;
}

std::string hex(std::uint32_t value, int digits = 8) {
    char text[16];
    std::snprintf(text, sizeof(text), "%0*X", digits, value);
    return text;
}

struct State {
    IsoImage *disc{};
    std::optional<DiscRange> range;
    std::optional<Directory> directory;
    p3rd::ModFolderFormat format{0u};
    std::unique_ptr<ArchiveView> view;
    std::unique_ptr<FileOverlay> overlay;
    std::unique_ptr<ModSession> session;
    std::shared_ptr<const Layout> layout; // null: the disc's own archive
    bool serving{};
    bool switched_live{};                        // a change was applied while running
    std::optional<Resolution> waiting;           // a switch waiting for a file's load to end
    std::optional<FileId> partial;               // a file the game has begun but not finished reading
    std::set<FileId> loaded_code;                // overlays read in full with memory writes due
    std::set<FileId> reported;                   // files whose problems were logged
    std::set<std::uint32_t> reserved;            // pages of guest memory held for mods' code
    std::map<std::string, std::uint32_t> placed; // code blocks with no address: where each went
    std::vector<std::uint32_t> pending_runs;     // mods' code to run at the next code load
    std::vector<std::string> problems;
};

State &state() {
    static State s;
    return s;
}

std::size_t raw_read(std::uint64_t offset, std::span<std::uint8_t> out) {
    State &s = state();
    if (!s.disc || !s.range || offset >= s.range->size) return 0u;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), s.range->size - offset));
    return s.disc->read(s.range->offset + offset, out.first(count));
}

std::string mod_name(const std::string &id) {
    State &s = state();
    if (s.session) {
        if (const Mod *mod = s.session->library().find(id)) return mod->name;
    }
    return id;
}

// Sizes the files would have with `resolution`, as the layout needs them.
std::map<FileId, std::uint64_t> sizes_for(const Resolution &resolution) {
    State &s = state();
    FileOverlay sizing(FileOverlay::Game{{}, [&s](FileId file) { return s.directory->size(file); }, {}});
    sizing.set(resolution);
    return sizing.sizes();
}

std::shared_ptr<const Layout> layout_for(const Resolution &resolution) {
    State &s = state();
    if (resolution.empty() || !s.directory) return nullptr;
    return std::make_shared<const Layout>(Layout::build(*s.directory, sizes_for(resolution)));
}

void log_resolution(const Resolution &resolution, const Layout *layout) {
    State &s = state();
    if (resolution.empty()) {
        std::cout << "[mods] no mod changes the game's files\n";
        return;
    }
    std::cout << "[mods] " << resolution.replacements.size() << " file(s) replaced, " << resolution.patches.size()
              << " patched";
    if (layout != nullptr && layout->moved(*s.directory))
        std::cout << "; DATA.BIN grows from " << s.directory->archive_bytes() << " to "
                  << layout->directory.archive_bytes() << " bytes";
    std::cout << "\n";
    if (!trace()) return;
    for (const auto &[file, source] : resolution.replacements) {
        std::cout << "[mods]   " << s.format.file_name(file) << " <- " << to_utf8(source.path) << " ("
                  << mod_name(source.mod) << ", " << (layout ? layout->directory.size(file) : 0u)
                  << " bytes, the disc's " << s.directory->size(file) << ")";
        if (layout != nullptr && layout->padded.contains(file)) std::cout << ", padded with zeros to its blocks";
        std::cout << "\n";
    }
    for (const auto &[file, patches] : resolution.patches)
        for (const Resolution::Source &source : patches)
            std::cout << "[mods]   " << s.format.file_name(file) << " patched by " << to_utf8(source.path) << " ("
                      << mod_name(source.mod) << ")\n";
    for (const Resolution::Conflict &conflict : resolution.conflicts) {
        std::cout << "[mods]   conflict on " << s.format.file_name(conflict.file) << ":";
        if (!conflict.winner.empty()) std::cout << " " << mod_name(conflict.winner) << " wins";
        for (const std::string &loser : conflict.overridden) std::cout << ", over " << mod_name(loser);
        for (const std::string &patcher : conflict.patched_by) std::cout << ", patched by " << mod_name(patcher);
        std::cout << "\n";
    }
}

void activate_now(const Resolution &resolution) {
    State &s = state();
    s.waiting.reset();
    if (!s.overlay || !s.view) return;
    s.overlay->set(resolution);
    s.layout = layout_for(resolution);
    s.view->set(s.layout, s.overlay.get());
    s.serving = s.layout != nullptr;
    s.reported.clear();
    s.loaded_code.clear();
    log_resolution(resolution, s.layout.get());
}

bool can_switch(const Resolution &next) {
    State &s = state();
    if (!s.directory) return false;
    // Code is in memory since the start; it cannot be taken back out.
    if (s.overlay && next.code != s.overlay->resolution().code) return false;
    // Added files and quests are taken over at start.
    if (s.overlay && (next.added != s.overlay->resolution().added || next.quests != s.overlay->resolution().quests))
        return false;
    const std::shared_ptr<const Layout> wanted = layout_for(next);
    const Layout disc = Layout::build(*s.directory, {});
    const Layout &now = s.layout ? *s.layout : disc;
    return (wanted ? *wanted : disc).same_as(now);
}

// Whether switching to `next` would change a file the game is halfway
// through reading. Streamed music is read a piece at a time all along, so only
// the files the switch changes count.
bool switch_waits(const Resolution &next) {
    State &s = state();
    if (!s.serving || !s.partial) return false;
    const FileId file = *s.partial;
    return s.overlay->touches(file) || next.replacements.contains(file) || next.patches.contains(file);
}

void activate(const Resolution &next) {
    State &s = state();
    if (s.session && s.overlay && s.overlay->resolution().same_files(next)) return;
    // A file the game is halfway through reading keeps its bytes to the end.
    if (switch_waits(next)) {
        s.waiting = next;
        return;
    }
    if (s.session) s.switched_live = true;
    activate_now(next);
}

void problem(const std::string &line) {
    std::cout << "[mods] " << line << "\n";
    state().problems.push_back(line);
}

void note_problems(FileId file) {
    State &s = state();
    if (s.reported.contains(file)) return;
    s.reported.insert(file);
    const std::shared_ptr<const FileContent> content = s.overlay->content(file);
    if (!content) return;
    for (const std::string &problem : content->problems) {
        const std::string line = s.format.file_name(file) + ": " + problem;
        std::cout << "[mods] " << line << "\n";
        s.problems.push_back(line);
    }
}

} // namespace

// A loaded file's memory blocks and code to run (defined below).
void write_after_load(psprecomp::Runtime &runtime, FileId file, const FileContent &content);

// Where a mod file's code blocks without an address go (defined below).
p3rd::Place placer(const fs::path &file);

fs::path mods_directory() {
    if (fs::path dir = environment_path("MHP3RD_MODS_DIR"); !dir.empty()) return dir;
    try {
        return install::user_data_directory() / "mods";
    } catch (const std::exception &) {
        return {};
    }
}

void attach_disc(IsoImage *disc) {
    State &s = state();
    s = State{};
    s.disc = disc;
    if (disc == nullptr) return;
    const std::optional<IsoImage::Entry> entry = disc->find(kDataBin);
    if (!entry || entry->directory) return;
    s.range = DiscRange{static_cast<std::uint64_t>(entry->lba) * IsoImage::kSectorSize, entry->size};

    // The directory is small: its first word says how many blocks it takes.
    std::vector<std::uint8_t> head(p3rd::kBlock);
    head.resize(raw_read(0u, head));
    std::vector<std::uint8_t> first(head);
    p3rd::decrypt(first, 0u, 0u);
    const std::uint32_t blocks = first.size() >= 4u
        ? static_cast<std::uint32_t>(first[0] | first[1] << 8u | first[2] << 16u | first[3] << 24u)
        : 0u;
    if (blocks > 0u && blocks < 512u) {
        std::vector<std::uint8_t> encrypted(static_cast<std::size_t>(blocks) * p3rd::kBlock);
        encrypted.resize(raw_read(0u, encrypted));
        s.directory = Directory::parse(encrypted, s.range->size);
    }
    if (!s.directory) {
        std::cerr << "[mods] DATA.BIN's directory could not be read; mods are off\n";
        return;
    }
    s.format.set_entries(static_cast<std::uint32_t>(s.directory->entries()));
    s.view = std::make_unique<ArchiveView>(*s.directory, &raw_read);
    s.overlay = std::make_unique<FileOverlay>(FileOverlay::Game{[&s](FileId file) { return s.view->original(file); },
        [&s](FileId file) { return s.directory->size(file); },
        [](FileId, std::vector<std::uint8_t> &bytes, const Resolution::Source &patch) {
            return p3rd::apply_patch(bytes, p3rd::read_blocks(patch, placer(patch.path)));
        }});

    ModSession::Paths paths;
    paths.folder = mods_directory();
    try {
        paths.choices = install::user_data_directory() / "mods.ini";
    } catch (const std::exception &) {
        paths.choices = paths.folder.parent_path() / "mods.ini";
    }
    paths.disabled_by = disabled_by();
    s.session = std::make_unique<ModSession>(s.format, paths, ModSession::Game{&can_switch, &activate});
    s.session->start();
    s.switched_live = false;
    if (paths.disabled_by != nullptr) std::cout << "[mods] off for this run: " << paths.disabled_by << " is set\n";
    if (trace())
        std::cout << "[mods] " << s.session->library().mods().size() << " mod(s) in " << to_utf8(paths.folder)
                  << "; choices in " << to_utf8(paths.choices) << "\n";
}

std::optional<DiscRange> data_bin_on_disc() {
    return state().range;
}

bool serving() {
    return state().serving;
}

std::uint64_t data_bin_size() {
    State &s = state();
    if (s.view && s.serving) return s.view->size();
    return s.range ? s.range->size : 0u;
}

std::optional<EntryAt> entry_at_offset(std::uint64_t offset) {
    State &s = state();
    if (!s.directory) return std::nullopt;
    const Directory &d = s.layout ? s.layout->directory : *s.directory;
    const std::uint64_t block = offset / p3rd::kBlock;
    if (block > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    const std::int64_t last = d.entry_at(static_cast<std::uint32_t>(block));
    if (last < 0 || static_cast<std::uint64_t>(last) >= d.entries()) return std::nullopt;
    const std::uint64_t start = static_cast<std::uint64_t>(d.blocks[last]) * p3rd::kBlock;
    const auto entry = static_cast<std::uint32_t>(last);
    const auto size = d.size(entry);
    if (offset - start >= size) return std::nullopt;
    return EntryAt{entry, offset - start, size};
}

std::optional<ArchiveEntry> read_archive_entry(std::uint32_t entry, std::size_t max_bytes) {
    State &s = state();
    if (!s.directory || !s.view) return std::nullopt;
    const Directory &d = s.layout ? s.layout->directory : *s.directory;
    if (entry >= d.entries()) return std::nullopt;
    const auto size = d.size(entry);
    if (size == 0u || size > max_bytes) return std::nullopt;
    ArchiveEntry result{static_cast<std::uint64_t>(d.blocks[entry]) * p3rd::kBlock,
        std::vector<std::uint8_t>(static_cast<std::size_t>(size))};
    if (s.view->read(result.offset, result.bytes) != size) return std::nullopt;
    return result;
}

// A read of the disc's own archive: native and script mods still hear of
// each whole file the game reads.
std::size_t unserved_read(std::uint64_t offset, std::span<std::uint8_t> out) {
    State &s = state();
    const std::size_t count = raw_read(offset, out);
    if (count == 0u || !s.directory || native::running() == 0u) return count;
    const Directory &d = *s.directory;
    const std::uint64_t end = offset + count;
    if (const std::int64_t last = d.entry_at(static_cast<std::uint32_t>((end - 1u) / p3rd::kBlock)); last >= 0) {
        const auto file = static_cast<FileId>(last);
        if (end >= static_cast<std::uint64_t>(d.blocks[file]) * p3rd::kBlock + d.size(file)) native::file_loaded(file);
    }
    return count;
}

std::size_t read_data_bin(std::uint64_t offset, std::span<std::uint8_t> out) {
    State &s = state();
    if (!s.serving || !s.view) return unserved_read(offset, out);
    if (s.waiting && !switch_waits(*s.waiting)) activate_now(*s.waiting);
    if (!s.serving) return unserved_read(offset, out);

    std::vector<FileId> touched;
    const std::size_t count = s.view->read(offset, out, &touched);
    if (count == 0u) return 0u;

    // Which file the read ended in, and whether the game has all of it now.
    const Directory &d = s.layout->directory;
    const std::uint64_t end = offset + count;
    s.partial.reset();
    if (const std::int64_t last = d.entry_at(static_cast<std::uint32_t>((end - 1u) / p3rd::kBlock)); last >= 0) {
        const auto file = static_cast<FileId>(last);
        // Which file each read ends in: how to find the id of a model the game shows.
        static const bool trace_reads = std::getenv("MHP3RD_TRACE_DATA_BIN") != nullptr;
        if (trace_reads) std::cout << "[mods] data " << s.format.file_name(file) << " (" << d.size(file) << " bytes)\n";
        const std::uint64_t file_end = static_cast<std::uint64_t>(d.blocks[file]) * p3rd::kBlock + d.size(file);
        if (end >= file_end) native::file_loaded(file);
        if (end < file_end)
            s.partial = file;
        else if (s.overlay->touches(file)) {
            const auto content = s.overlay->content(file);
            // An overlay's writes wait for the cache flush that ends its
            // load; any other file is in memory now that it is read.
            if (content && (!content->after_load.empty() || !content->run.empty())) {
                if (p3rd::overlay_image(content->bytes))
                    s.loaded_code.insert(file);
                else
                    write_after_load(kernel().runtime(), file, *content);
            }
        }
    }
    if (!touched.empty()) {
        FileId previous = ~FileId{};
        for (const FileId file : touched) {
            if (file == previous) continue;
            previous = file;
            note_problems(file);
            if (!trace()) continue;
            const std::uint64_t start = static_cast<std::uint64_t>(d.blocks[file]) * p3rd::kBlock;
            const std::uint64_t from = offset > start ? offset - start : 0u;
            const Resolution &r = s.overlay->resolution();
            std::string source;
            if (const auto found = r.replacements.find(file); found != r.replacements.end())
                source = "replaced by " + mod_name(found->second.mod);
            if (const auto found = r.patches.find(file); found != r.patches.end()) {
                for (const Resolution::Source &patch : found->second)
                    source += (source.empty() ? "patched by " : ", patched by ") + mod_name(patch.mod);
            }
            std::cout << "[mods] read " << s.format.file_name(file) << " +" << from << " "
                      << std::min<std::uint64_t>(end, start + d.span(file)) - (start + from) << " of " << d.size(file)
                      << " bytes: " << source << "\n";
        }
    }
    return count;
}

// Writes a loaded file's memory blocks (FileContent::after_load) and queues
// its code to run. An overlay must be the one in its slot now, and its
// recompiled code goes in before any of it changes, so that only the units
// the mods change leave native code.
void write_after_load(psprecomp::Runtime &runtime, FileId file, const FileContent &content) {
    State &s = state();
    auto &memory = runtime.memory();
    if (const std::optional<p3rd::OverlayImage> image = p3rd::overlay_image(content.bytes)) {
        for (std::uint32_t i = 0; i < 8u; ++i) {
            if (memory.load8(image->load + i) != content.bytes[i]) return;
        }
        const bool changes_code =
            std::any_of(content.after_load.begin(), content.after_load.end(), [&image](const MemoryWrite &write) {
                return write.address >= image->load && write.address < image->code_end;
            });
        if (changes_code && !install_overlay_at(runtime, image->load))
            problem(s.format.file_name(file) + ": its overlay has no recompiled code; it runs in the interpreter");
    }
    for (const MemoryWrite &write : content.after_load) {
        if (!write.expect.empty()) {
            std::vector<std::uint8_t> found(write.expect.size());
            for (std::size_t i = 0; i < found.size(); ++i)
                found[i] = memory.load8(write.address + static_cast<std::uint32_t>(i));
            if (const auto wrong = p3rd::mismatch(write.expect, found)) {
                problem(s.format.file_name(file) + ": the write at 0x" + hex(write.address) + " is skipped: " + *wrong);
                continue;
            }
        }
        memory.copy_in(write.address, write.bytes);
        std::vector<std::string> lines;
        p3rd::retire_code(runtime, write.address, write.bytes.size(), s.format.file_name(file) + "P", lines);
        for (const std::string &line : lines) problem(line);
        if (trace())
            std::cout << "[mods] after loading " << s.format.file_name(file) << ": wrote " << write.bytes.size()
                      << " bytes at 0x" << hex(write.address) << "\n";
    }
    s.pending_runs.insert(s.pending_runs.end(), content.run.begin(), content.run.end());
}

std::vector<std::uint32_t> code_loaded(psprecomp::Runtime &runtime) {
    State &s = state();
    if (s.overlay) {
        for (const FileId file : s.loaded_code) {
            if (const std::shared_ptr<const FileContent> content = s.overlay->content(file))
                write_after_load(runtime, file, *content);
        }
    }
    s.loaded_code.clear();
    // Tables mods made longer, in the overlays that have just loaded, and
    // mods' quests in the event quest list.
    tables::overlay_loaded(runtime);
    quests::overlay_loaded(runtime);
    // Code mods' blocks from the start, and the loaded files': the game is
    // running and at a point where it expects code to have changed.
    std::vector<std::uint32_t> run = std::move(s.pending_runs);
    s.pending_runs.clear();
    return run;
}

// Memory a mod's code was written to, kept from the game's allocator: each
// run of free pages is allocated at its address. The executable's own image
// needs nothing; memory already in use is reported, since the game may
// overwrite the code there.
//
// The 16 KiB below the executable are where the community loader keeps mods'
// code, and plain code files put it there at fixed addresses. No allocator
// hands that memory out, so it needs no reserving, except the first bytes,
// which hold Yakumo's own stubs.
p3rd::Reservation reserve_memory(std::uint32_t address, std::uint32_t size) {
    constexpr std::uint32_t kPage = 0x100u;
    State &s = state();
    Kernel &k = kernel();
    const std::uint32_t image_end = k.user_memory_start();
    const std::uint64_t end = static_cast<std::uint64_t>(address) + size;
    if (address < kHostStubsEnd && end > kThreadExitStub)
        return {false, "would overwrite Yakumo's own code at 0x" + hex(kThreadExitStub) + "; it is skipped"};
    const auto needs = [&](std::uint32_t page) {
        return !s.reserved.contains(page) && !(page >= kThreadExitStub && page + kPage <= image_end);
    };
    for (std::uint32_t page = address & ~(kPage - 1u); page < end;) {
        if (!needs(page)) {
            page += kPage;
            continue;
        }
        std::uint32_t run = page;
        while (run < end && needs(run)) run += kPage;
        if (k.allocate_block("mod code", 2u, run - page, page) < 0)
            return {true,
                "is in memory the game or Yakumo already uses (0x" + hex(page) + " to 0x" + hex(run) +
                    "); the game may overwrite it"};
        for (std::uint32_t at = page; at < run; at += kPage) s.reserved.insert(at);
        page = run;
    }
    return {};
}

// A code block that names no address, a main block of the loader's typed
// files: allocated once per file and id from the top of free memory, and
// reused each time the file is read again.
std::optional<std::uint32_t> place_code(const fs::path &file, const std::string &id, std::uint32_t size) {
    State &s = state();
    const std::string key = to_utf8(file) + "|" + id;
    if (const auto found = s.placed.find(key); found != s.placed.end()) return found->second;
    Kernel &k = kernel();
    const std::int32_t uid = k.allocate_block("mod code " + id, 4u, size == 0u ? 1u : size, 16u);
    if (uid < 0) return std::nullopt;
    const MemoryBlock *block = k.find_block(uid);
    if (block == nullptr) return std::nullopt;
    for (std::uint32_t at = block->address & ~0xFFu; at < block->address + block->size; at += 0x100u)
        s.reserved.insert(at);
    s.placed[key] = block->address;
    if (trace())
        std::cout << "[mods] " << id << " from " << to_utf8(file.filename()) << " placed at 0x" << hex(block->address)
                  << "\n";
    return block->address;
}

p3rd::Place placer(const fs::path &file) {
    return [file](const std::string &id, std::uint32_t size) { return place_code(file, id, size); };
}

void write_code(psprecomp::Runtime &runtime) {
    State &s = state();
    if (!s.overlay || s.overlay->resolution().code.empty()) return;
    std::vector<p3rd::CodeFile> files;
    for (const Resolution::Source &source : s.overlay->resolution().code)
        files.push_back({mod_name(source.mod) + " (" +
                (source.edits.empty() ? to_utf8(source.path.filename()) : std::string("[Memory]")) + ")",
            source});
    p3rd::CodeReport report;
    for (const p3rd::CodeFile &file : files) {
        p3rd::CodeReport one = p3rd::write_code(runtime, {file}, &reserve_memory, placer(file.source.path), trace());
        report.blocks += one.blocks;
        report.bytes += one.bytes;
        report.units += one.units;
        report.run.insert(report.run.end(), one.run.begin(), one.run.end());
        report.problems.insert(report.problems.end(), one.problems.begin(), one.problems.end());
    }
    s.pending_runs = report.run;
    for (const std::string &line : report.problems) problem(line);
    std::cout << "[mods] code: " << report.blocks << " block(s), " << report.bytes << " bytes written; " << report.units
              << " unit(s) of recompiled code retired, compiled again from memory by the JIT\n";
}

void start_native(psprecomp::Runtime &runtime) {
    State &s = state();
    if (!s.overlay) return;
    std::map<FileId, Resolution::Source> files = s.overlay->resolution().added;
    if (!s.overlay->resolution().quests.empty()) {
        try {
            const fs::path cache = install::user_data_directory() / "mods-cache" / "quests";
            (void)quests::install(runtime, s.overlay->resolution().quests, files, cache);
        } catch (const std::exception &error) {
            problem(std::string("Quests from mods cannot be prepared: ") + error.what());
        }
    }
    (void)added::install(runtime, files);
    native::start(runtime, s.overlay->resolution().native);
    scripts::start(runtime, s.overlay->resolution().scripts);
}

std::optional<std::vector<std::uint8_t>> original_file(FileId file) {
    State &s = state();
    if (!s.view || !s.directory || file >= s.directory->entries()) return std::nullopt;
    return s.view->original(file);
}

ModSession *session() {
    return state().session.get();
}

std::string pending_note() {
    State &s = state();
    if (!s.session) return {};
    if (s.session->restart_pending())
        return "Restart Yakumo to apply: a file changes size in a way the running game cannot take.";
    if (s.waiting) return "Applies once the game finishes loading the file it is reading.";
    if (s.switched_live)
        return "Applied: each file changes the next time the game loads it, usually at the next area or menu. "
               "Files it loads only once at start need a restart.";
    return {};
}

const std::vector<std::string> &problems() {
    return state().problems;
}

} // namespace mhp3rd::mods
