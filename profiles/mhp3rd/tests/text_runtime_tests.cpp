// Runtime contracts over synthetic archive metadata, encrypted invented bytes
// and guest RAM. The real decrypt/parser/frame paths run without a game image.
#include "text/translation.hpp"
#include "mods/mhp3rd_data_bin.hpp"
#include "mods/mhp3rd_mods.hpp"
#include "psprecomp/guest_memory.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
std::uint32_t entry = 4289;
std::uint64_t entry_size = 64;
constexpr std::uint64_t archive_start = 2048;
constexpr std::uint32_t ram = 0x08800000;
std::optional<mhp3rd::mods::ArchiveEntry> catalog;

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
void word(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(at + i) = static_cast<std::uint8_t>(value >> (i * 8));
}
}

namespace mhp3rd::mods {
std::optional<ArchiveEntry> read_archive_entry(std::uint32_t id, std::size_t max_bytes) {
    if (id == entry && catalog && catalog->bytes.size() <= max_bytes) return catalog;
    return std::nullopt;
}
// An isolated lookup seam: this test owns one synthetic archive entry.
std::optional<EntryAt> entry_at_offset(std::uint64_t offset) {
    if (offset < archive_start || offset - archive_start >= entry_size) return std::nullopt;
    return EntryAt{entry, offset - archive_start, entry_size};
}
}

int main(int argc, char **argv) {
    namespace fs = std::filesystem;
    using namespace mhp3rd;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() / ("yakumo-text-runtime-" + std::to_string(unique));
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{dir};
    try {
        fs::create_directories(dir);
        {
            std::ofstream out(dir / "test.lang");
            out << "language = test\n[4289]\n0:0 = Translated\n[2835]\n2:1 = New\n";
        }
        // Display-latency regression: several loaded quest lists must not
        // delay a newly opened screen until a full sweep of guest RAM finishes.
        // Thirty game frames is a one-second upper bound, not a CPU benchmark.
        if (argc > 1 && std::string(argv[1]) == "--quest-display-latency") {
            const std::vector<std::uint32_t> entries{
                4059, 4060, 4061, 4062, 4063, 4064, 4065, 4066, 4070, 4071, 4072, 4073};
            std::ofstream language(dir / "latency.lang");
            language << "language = latency\n";
            for (const auto id : entries)
                language << '[' << id << "]\n64:136 = Title\n68:160 = Goal\n72:184 = Details\n";
            language.close();
            text::set_language("latency", {dir});
            psprecomp::GuestMemory live(64u * 1024u * 1024u);
            const text::ArenaAllocator arena = [&](std::size_t size) {
                return std::optional<text::Arena>{
                    {ram + 0x01ff0000u, ram + 0x01ff0000u + static_cast<std::uint32_t>(size)}};
            };
            std::vector<std::uint32_t> copies;
            std::vector<std::vector<std::uint8_t>> active_sources;
            std::vector<bool> active_seen(entries.size(), false);
            for (std::size_t n = 0; n < entries.size(); ++n) {
                entry = entries[n];
                entry_size = 512;
                std::vector<std::uint8_t> source(512, 0);
                word(source, 0, 64);
                word(source, 4, 256);
                word(source, 64 + 28, 101 + n);
                word(source, 256, 328);
                word(source, 260, 352);
                word(source, 264, 376);
                word(source, 268, 256);
                const std::string unused = "Unused" + std::to_string(n + 100);
                std::copy(unused.begin(), unused.end(), source.begin() + 328);
                std::copy(unused.begin(), unused.end(), source.begin() + 352);
                std::copy(unused.begin(), unused.end(), source.begin() + 376);
                const std::vector<std::string> fields{"SourceTitle" + std::to_string(n + 100),
                    "SourceGoal" + std::to_string(n + 100), "SourceDetails" + std::to_string(n + 100)};
                for (std::size_t i = 0; i < fields.size(); ++i) {
                    word(source, 64 + i * 4, 136 + static_cast<std::uint32_t>(i) * 24);
                    std::copy(fields[i].begin(), fields[i].end(), source.begin() + 136 + i * 24);
                }
                word(source, 76, 64); // Table sentinel, never translated.
                copies.push_back(ram + 0x01400000u + static_cast<std::uint32_t>(n) * 1024u);
                live.copy_in(copies.back(), source);
                active_sources.push_back(source);
                mods::p3rd::encrypt(source, 1, 0);
                text::translate_read(archive_start, source);
                const auto request = ram + 4096;
                live.store16(request + 2, static_cast<std::uint16_t>(entry));
                live.store32(request + 4, copies.back());
                live.store32(request + 8, static_cast<std::uint32_t>(entry_size));
                live.store32(request + 12, 0);
                text::note_loader_completion(live, 0x08865840u, request);
            }
            const auto matches = [&](std::uint32_t address, const std::string &expected) {
                if (!live.contains(address, expected.size() + 1)) return false;
                for (std::size_t i = 0; i < expected.size(); ++i)
                    if (live.load8(address + static_cast<std::uint32_t>(i)) != static_cast<std::uint8_t>(expected[i]))
                        return false;
                return live.load8(address + static_cast<std::uint32_t>(expected.size())) == 0;
            };
            unsigned ready = 0;
            for (unsigned frame = 0; frame < 30; ++frame) {
                // One active quest at a time, as in the game; visit every
                // record while all twelve catalogs remain loaded.
                const auto active = frame % entries.size();
                live.copy_in(text::kActiveQuestBlock, active_sources[active]);
                text::frame(live, arena);
                const auto root = text::kActiveQuestBlock;
                active_seen[active] = matches(root + live.load32(root + 64), "Title") &&
                    matches(root + live.load32(root + 68), "Goal") && matches(root + live.load32(root + 72), "Details");
                if (std::getenv("MHP3RD_TEXT_SEARCH_UNLIMITED") == nullptr)
                    require(text::search_work_last_frame().bytes <= text::kSearchBytesPerFrame,
                        "quest display latency must not be fixed by unbounded RAM searches");
                ready = 0;
                for (std::size_t n = 0; n < entries.size(); ++n) {
                    const auto title = copies[n] + live.load32(copies[n] + 64);
                    const auto goal = copies[n] + live.load32(copies[n] + 68);
                    const auto details = copies[n] + live.load32(copies[n] + 72);
                    if (matches(title, "Title") && matches(goal, "Goal") && matches(details, "Details") &&
                        active_seen[n])
                        ++ready;
                    require(live.load32(copies[n] + 76) == 64, "quest sentinel remains unchanged");
                }
                if (ready == entries.size()) break;
            }
            std::cout << "Quest screens ready within 30 frames: " << ready << '/' << entries.size() << '\n';
            require(
                ready == entries.size(), "quest lists and each visited active quest must translate within 30 frames");
            return 0;
        }
        for (std::size_t n = 0; n < 16; ++n) {
            text::set_language("test", {dir});
            std::vector<std::uint8_t> bytes(n, 0);
            mods::p3rd::encrypt(bytes, 1, 0);
            text::translate_read(archive_start, bytes);
        }
        // A truncated ordinary header with no valid table must also be safe.
        entry = 2835;
        text::set_language("test", {dir});
        std::vector<std::uint8_t> header(12, 0);
        word(header, 0, 62);
        word(header, 4, 8);
        word(header, 8, 0xffffffffu);
        mods::p3rd::encrypt(header, 1, 0);
        text::translate_read(archive_start, header);
        entry = 4289;
        text::set_language("test", {dir});
        std::vector<std::uint8_t> clear(64, 0);
        word(clear, 0, 0);
        word(clear, 4, 16);
        word(clear, 8, 0xffffffffu);
        word(clear, 16, 0);
        word(clear, 20, 16);
        word(clear, 24, 0xffffffffu);
        const std::string original = "Original";
        std::copy(original.begin(), original.end(), clear.begin() + 32);
        auto encrypted = clear;
        mods::p3rd::encrypt(encrypted, 1, 0);
        psprecomp::GuestMemory memory(64u * 1024u * 1024u);
        memory.copy_in(ram, clear);
        unsigned allocations = 0;
        const text::ArenaAllocator allocate = [&](std::size_t size) {
            ++allocations;
            return std::optional<text::Arena>{{ram + 4096, ram + 4096 + static_cast<std::uint32_t>(size)}};
        };
        text::translate_read(archive_start, std::span(encrypted).first(2));
        text::frame(memory, allocate);
        require(allocations == 0, "a short prefix does not trigger translation");
        // Tail before the middle: no fabricated unread bytes reach the parser.
        text::translate_read(archive_start + 32, std::span(encrypted).subspan(32));
        text::frame(memory, allocate);
        require(allocations == 0, "a gap does not trigger translation");
        text::translate_read(archive_start + 2, std::span(encrypted).subspan(2, 30));
        text::frame(memory, allocate);
        require(allocations == 1 && text::applied_blocks().size() == 1, "split dialogue is applied once");
        const auto translated = ram + 16 + memory.load32(ram + 20);
        require(memory.load8(translated) == 'T', "the live dialogue points into the translated arena");
        memory.copy_in(ram, clear);
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(allocations == 1, "reloading a dialogue reuses its arena");
        require(memory.load8(ram + 16 + memory.load32(ram + 20)) == 'T', "reload reapplies translation");
        // Ordinary menu blocks are located by their header and probe string.
        entry = 2835;
        entry_size = 128;
        clear.assign(128, 0);
        word(clear, 0, 2);
        word(clear, 4, 8);
        word(clear, 8, 32);
        word(clear, 32, 12);
        word(clear, 36, 16);
        word(clear, 40, 0xffffffffu);
        const std::string menu = "Menu";
        std::copy(menu.begin(), menu.end(), clear.begin() + 48);
        memory.copy_in(ram, clear);
        encrypted = clear;
        mods::p3rd::encrypt(encrypted, 1, 0);
        text::set_language("test", {dir});
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(text::applied_blocks().size() == 1, "ordinary text blocks are located and applied");
        require(memory.load8(ram + 32 + memory.load32(ram + 36)) == 'N', "ordinary block points to translated text");
        memory.copy_in(ram, clear);
        text::forget_blocks();
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(memory.load8(ram + 32 + memory.load32(ram + 36)) == 'N', "forgotten blocks can be reapplied");
        {
            std::ofstream out(dir / "test.lang");
            out << "language = test\n[16]\n2:1 = Main\n";
        }
        text::set_language("test", {dir});
        memory.copy_in(text::kMainTextBlock, clear);
        text::frame(memory, allocate);
        require(text::applied_blocks().at(0).entry == 16, "main text needs no archive read hook");
        // Two invented quest records, copied as an archive buffer and as inline
        // fields. The parser must protect sentinels and patch both shapes.
        {
            std::ofstream out(dir / "test.lang");
            out << "language = test\n[4059]\n64:136 = NewTitle\n68:160 = Goal\n72:184 = Details\n";
        }
        entry = 4059;
        entry_size = 512;
        clear.assign(512, 0);
        word(clear, 0, 64);
        word(clear, 4, 256);
        const std::vector<std::string> fields{"FirstTitle", "Objective", "Description"};
        for (std::size_t record = 0; record < 2; ++record) {
            const auto base = record == 0 ? 64u : 256u;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                const auto offset = base + 72 + i * 24;
                word(clear, base + i * 4, offset);
                std::copy(fields[i].begin(), fields[i].end(), clear.begin() + offset);
            }
            word(clear, base + 12, base);
        }
        memory.copy_in(ram, clear);
        memory.copy_in(ram + 1024, clear);
        // A separate inline structure preserves the source spacing.
        memory.copy_in(ram + 2048, std::span(clear).subspan(136, 72));
        encrypted = clear;
        mods::p3rd::encrypt(encrypted, 1, 0);
        text::set_language("test", {dir});
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(text::applied_blocks().at(0).applied == 6, "both quest buffer copies are repointed");
        require(memory.load32(ram + 76) == 64, "quest table sentinels are preserved");
        for (unsigned frame = 0; frame < 260 && memory.load8(ram + 2048) != 'N'; ++frame) {
            text::frame(memory, allocate);
            require(text::search_work_last_frame().bytes <= text::kSearchBytesPerFrame,
                "quest copy and inline searches share a bounded frame budget");
        }
        require(memory.load8(ram + 2048) == 'N', "inline quest fields are patched in their own slots");
        require(memory.load8(ram + 2072) == 'G', "the inline objective is translated");
        // A custom catalog may contain just one quest. Its next record word
        // is the zero terminator, not a second offset.
        auto single = clear;
        word(single, 4, 0);
        for (bool relocated : {false, true}) {
            text::set_language("test", {dir});
            auto loaded = single;
            if (relocated) word(loaded, 0, ram + 64);
            memory.copy_in(ram, loaded);
            encrypted = single;
            mods::p3rd::encrypt(encrypted, 1, 0);
            text::translate_read(archive_start, encrypted);
            text::frame(memory, allocate);
            const auto pointer = memory.load32(ram + 64);
            require(pointer != 136 && memory.contains(ram + pointer, 9),
                "single-record quest is discovered with relative or relocated record pointers");
            for (std::size_t i = 0; i < 9; ++i)
                require(memory.load8(ram + pointer + i) == static_cast<std::uint8_t>("NewTitle"[i]),
                    "single-record quest title matches completely including its terminator");
            require(memory.load32(ram + 4) == 0 && memory.load32(ram + 76) == 64,
                "single-record discovery preserves both sentinels");
        }
        for (std::uint32_t second : {1u, 512u, 0xFFFFFFFFu}) {
            text::set_language("test", {dir});
            auto invalid = single;
            word(invalid, 4, second);
            memory.copy_in(ram, invalid);
            encrypted = single;
            mods::p3rd::encrypt(encrypted, 1, 0);
            text::translate_read(archive_start, encrypted);
            text::frame(memory, allocate);
            require(memory.load32(ram + 64) == 136,
                "single-record discovery rejects a nonzero invalid next-record pointer");
        }
        // Completion hints must identify a whole, in-bounds quest load from
        // the supported caller. Invalid notifications cannot bypass discovery.
        for (unsigned variant = 0; variant < 9; ++variant) {
            text::set_language("test", {dir});
            const auto destination = ram + 20u * 1024u * 1024u;
            const auto request = ram + 4096;
            memory.copy_in(destination, clear);
            encrypted = clear;
            mods::p3rd::encrypt(encrypted, 1, 0);
            text::translate_read(archive_start, variant == 8 ? std::span(encrypted).first(64) : std::span(encrypted));
            memory.store16(request + 2, variant == 2 ? 65535 : 4059);
            memory.store32(request + 4, variant == 3 ? destination + 1 : variant == 4 ? 0xFFFFFFFFu : destination);
            memory.store32(request + 8, variant == 5 ? 128 : 512);
            memory.store32(request + 12, variant == 6 ? 512 : 0);
            text::note_loader_completion(memory, variant == 1 ? 0 : 0x08865840u, variant == 7 ? 0xFFFFFFF0u : request);
            text::frame(memory, allocate);
            require((memory.load32(destination + 64) != 136) == (variant == 0),
                "only a valid complete loader notification translates the distant catalog immediately");
        }
        // The active quest is a separate container, loaded without a catalog
        // read. Its record and string offsets differ from the catalog's.
        {
            std::ofstream out(dir / "test.lang");
            out << "language = test\n[4059]\n64:136 = A title longer than its original slot\n"
                   "68:160 = Goal\n72:184 = Details\n";
        }
        auto source = clear;
        word(source, 64 + 28, 101);
        auto encoded_catalog = source;
        mods::p3rd::encrypt(encoded_catalog, 1, 0);
        catalog = mods::ArchiveEntry{archive_start, encoded_catalog};
        auto active = source;
        // Move the first record by 32 bytes within the active container.
        std::copy_n(source.begin() + 64, 192, active.begin() + 96);
        word(active, 0, 96);
        for (std::size_t n = 0; n < 3; ++n) word(active, 96 + n * 4, 168 + n * 24);
        word(active, 108, 96);
        const auto quest_root = text::kActiveQuestBlock;
        for (unsigned reload = 0; reload < 2; ++reload) {
            if (reload == 0) text::set_language("test", {dir});
            memory.copy_in(quest_root, active);
            text::frame(memory, allocate);
            const std::string expected = "A title longer than its original slot";
            const auto title = quest_root + memory.load32(quest_root + 96);
            require(memory.contains(title, expected.size() + 1), "active quest points into valid guest memory");
            for (std::size_t n = 0; n <= expected.size(); ++n)
                require(memory.load8(title + n) == (n == expected.size() ? 0 : expected[n]),
                    "active quest translates on its first frame without a catalog read");
            require(memory.load32(quest_root + 108) == 96, "active quest preserves the table sentinel");
            require(memory.load8(quest_root + 168) == 'F', "active quest preserves its inline source text");
        }
        for (unsigned failure = 0; failure < 5; ++failure) {
            text::set_language("test", {dir});
            auto bad = active;
            if (failure == 0) word(bad, 0, 0xFFFFFFFCu);
            if (failure == 1) word(bad, 96 + 28, 999);
            if (failure == 2) word(bad, 108, 0);
            if (failure == 3) bad[168] = 'X';
            if (failure == 4) word(bad, 100, 0xFFFFFFFFu);
            memory.copy_in(quest_root, bad);
            text::frame(memory, allocate);
            for (std::size_t n = 0; n < bad.size(); ++n)
                require(memory.load8(quest_root + n) == bad[n],
                    "invalid active quest metadata or source text is not partially patched");
        }
        catalog.reset();
        // Search work is bounded over full RAM, including absent probes. A
        // second loaded entry must progress rather than starve behind the first.
        {
            std::ofstream out(dir / "test.lang");
            out << "language = test\n[4059]\n64:136 = NewTitle\n68:160 = Goal\n72:184 = Details\n"
                   "[4061]\n64:136 = Other\n[4289]\n0:0 = Greeting\n[2835]\n2:1 = Label\n";
        }
        psprecomp::GuestMemory searched(64u * 1024u * 1024u);
        unsigned search_allocations = 0;
        const auto allocate_search = [&](std::size_t size) {
            ++search_allocations;
            return std::optional<text::Arena>{
                {ram + 0x01ff0000u, ram + 0x01ff0000u + static_cast<std::uint32_t>(size)}};
        };
        const auto checked_frame = [&] {
            text::frame(searched, allocate_search);
            require(text::search_work_last_frame().bytes <= text::kSearchBytesPerFrame,
                "all pending searches share one frame budget");
        };
        const auto until = [&](const auto &ready, unsigned limit) {
            for (unsigned frame = 0; frame < limit && !ready(); ++frame) checked_frame();
            require(ready(), "a bounded search eventually discovers its target");
        };
        text::set_language("test", {dir});
        entry = 4059;
        text::translate_read(archive_start, encrypted);
        entry = 4061;
        auto other = clear;
        other[136] = 'S';
        auto other_encrypted = other;
        mods::p3rd::encrypt(other_encrypted, 1, 0);
        text::translate_read(archive_start, other_encrypted);
        // The first title begins four bytes before a slice edge. The second
        // archive is in a later slice and belongs to a different pending entry.
        const auto boundary_copy = ram + text::kSearchBytesPerFrame - 140u;
        const auto later_copy = ram + 3u * text::kSearchBytesPerFrame;
        searched.copy_in(boundary_copy, clear);
        searched.copy_in(later_copy, other);
        until([&] { return searched.load32(boundary_copy + 64u) != 136u && searched.load32(later_copy + 64u) != 136u; },
            16);
        require(searched.load32(boundary_copy + 76u) == 64u, "cross-slice copies retain their sentinel");
        // Unaligned inline candidates cross slice edges too. Comparing their
        // whole fields must not be restricted to a slice's byte span.
        const auto inline_at = ram + 2u * text::kSearchBytesPerFrame - 3u;
        searched.copy_in(inline_at, std::span(clear).subspan(136, 72));
        until([&] { return searched.load8(inline_at) == 'N'; }, 600);
        require(searched.load8(inline_at + 24u) == 'G', "cross-slice inline objective is intact");
        // A new structure behind the cursor after an earlier hit still needs
        // a later pass; stopping all retries at the first hit would lose it.
        const auto late_inline = ram + 8192u;
        searched.copy_in(late_inline, std::span(clear).subspan(136, 72));
        until([&] { return searched.load8(late_inline) == 'N'; }, 900);
        // A reread resets both cursors and reuses its existing arena.
        const auto arena_allocations = search_allocations;
        entry = 4059;
        searched.copy_in(boundary_copy, clear);
        text::translate_read(archive_start, encrypted);
        until([&] { return searched.load32(boundary_copy + 64u) != 136u; }, 16);
        require(search_allocations == arena_allocations, "incremental reloading reuses its existing arena");
        // No file initially exists; place it behind a completed portion of the
        // scan. A later complete pass must find it, not count each slice as a
        // failed attempt and exhaust the old retry limit prematurely.
        text::set_language("test", {dir});
        psprecomp::GuestMemory absent(64u * 1024u * 1024u);
        entry = 4059;
        text::translate_read(archive_start, encrypted);
        for (unsigned frame = 0; frame < 4; ++frame) {
            text::frame(absent, allocate_search);
            require(text::search_work_last_frame().bytes <= text::kSearchBytesPerFrame,
                "missing quest probes are bounded too");
        }
        absent.copy_in(ram + 1024u, clear);
        for (unsigned frame = 0; frame < 260 && absent.load32(ram + 1088u) == 136u; ++frame) {
            text::frame(absent, allocate_search);
            require(text::search_work_last_frame().bytes <= text::kSearchBytesPerFrame,
                "retrying a missing probe preserves the frame budget");
        }
        require(absent.load32(ram + 1088u) != 136u, "late archive copies behind the cursor are found on retry");
        // The last inline structure ends at the RAM boundary. The final partial
        // slice and its candidate verification must remain in bounds.
        const auto end_inline = ram + 0x02000000u - 72u;
        absent.copy_in(end_inline, std::span(clear).subspan(136, 72));
        for (unsigned frame = 0; frame < 260 && absent.load8(end_inline) != 'N'; ++frame)
            text::frame(absent, allocate_search);
        require(absent.load8(end_inline) == 'N', "inline fields at the RAM boundary are found safely");
        // A purported final field without room for its terminator is not a
        // complete structure and must not cause a write outside the RAM span.
        const auto unterminated = ram + 0x02000000u - 59u;
        absent.copy_in(unterminated, std::span(clear).subspan(136, 59));
        for (unsigned frame = 0; frame < 260; ++frame) text::frame(absent, allocate_search);
        require(absent.load8(unterminated) == 'F', "an unterminated field at the RAM edge is left alone");
        // Dialogue/menu probes also yield instead of doing an independent
        // full-RAM scan. Verify a dialogue probe that spans a slice boundary.
        text::set_language("test", {dir});
        psprecomp::GuestMemory probe_memory(64u * 1024u * 1024u);
        entry = 4289;
        entry_size = 64;
        std::vector<std::uint8_t> dialogue(64, 0);
        word(dialogue, 4, 16);
        word(dialogue, 8, 0xffffffffu);
        word(dialogue, 20, 16);
        word(dialogue, 24, 0xffffffffu);
        const std::string greeting = "Greeting source";
        std::copy(greeting.begin(), greeting.end(), dialogue.begin() + 32);
        auto encoded_dialogue = dialogue;
        mods::p3rd::encrypt(encoded_dialogue, 1, 0);
        const auto dialogue_at = ram + text::kSearchBytesPerFrame - 36u;
        probe_memory.copy_in(dialogue_at, dialogue);
        text::translate_read(archive_start, encoded_dialogue);
        text::frame(probe_memory, allocate_search);
        require(probe_memory.load32(dialogue_at + 20u) != 16u, "a dialogue probe can cross a slice boundary");
        entry = 2835;
        entry_size = 128;
        std::vector<std::uint8_t> block(128, 0);
        word(block, 0, 2);
        word(block, 4, 8);
        word(block, 8, 32);
        word(block, 32, 12);
        word(block, 36, 16);
        word(block, 40, 0xffffffffu);
        const std::string label = "Menu source";
        std::copy(label.begin(), label.end(), block.begin() + 48);
        auto encoded_block = block;
        mods::p3rd::encrypt(encoded_block, 1, 0);
        const auto block_at = ram + 3u * text::kSearchBytesPerFrame;
        probe_memory.copy_in(block_at, block);
        text::translate_read(archive_start, encoded_block);
        text::frame(probe_memory, allocate_search);
        require(probe_memory.load32(block_at + 36u) == 16u, "a far menu waits for its search slice");
        for (unsigned frame = 0; frame < 8 && probe_memory.load32(block_at + 36u) == 16u; ++frame) {
            text::frame(probe_memory, allocate_search);
            require(text::search_work_last_frame().bytes <= text::kSearchBytesPerFrame,
                "menu probes share the bounded work contract");
        }
        require(probe_memory.load32(block_at + 36u) != 16u, "a menu in a later slice is translated");
        // Diagnostic comparison can restore a complete blocking search without
        // dropping any records or changing the normal configured budget.
        text::set_language("test", {dir});
        probe_memory.copy_in(block_at, block);
        text::translate_read(archive_start, encoded_block);
#ifdef _WIN32
        _putenv_s("MHP3RD_TEXT_SEARCH_UNLIMITED", "1");
#else
        setenv("MHP3RD_TEXT_SEARCH_UNLIMITED", "1", 1);
#endif
        text::frame(probe_memory, allocate_search);
        require(probe_memory.load32(block_at + 36u) != 16u &&
                text::search_work_last_frame().bytes > text::kSearchBytesPerFrame,
            "the diagnostic switch restores an unsliced probe scan");
#ifdef _WIN32
        _putenv_s("MHP3RD_TEXT_SEARCH_UNLIMITED", "");
#else
        unsetenv("MHP3RD_TEXT_SEARCH_UNLIMITED");
#endif
        const auto current_allocations = allocations;
        text::set_language("original", {dir});
        require(!text::active(), "original mode clears the translation state");
        text::translate_read(archive_start, encrypted);
        text::frame(memory, allocate);
        require(allocations == current_allocations, "inactive mode performs no allocation");
        std::cout << "text runtime tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
