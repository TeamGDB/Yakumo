#include "mods/mhp3rd_added_files.hpp"

#include "mods/mod_ini.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/jit.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

namespace mhp3rd::mods::added {
namespace {

namespace fs = std::filesystem;
using psprecomp::AllegrexContext;
using psprecomp::Runtime;

constexpr std::uint32_t kSize = 0x08864E8Cu;   // size(loader, id): bytes
constexpr std::uint32_t kSubmit = 0x08863CDCu; // submit(loader, id, destination, ...): 1
constexpr std::uint32_t kRead = 0x08864C54u;   // read(loader, id, destination, length or -1): bytes

struct CodeWord {
    std::uint32_t address;
    std::uint32_t word;
};
// Instructions of all three, as traced on NPJB-40001.
constexpr std::array<CodeWord, 11> kSignature{{
    {0x08864E8Cu, 0x30A5FFFFu}, // andi a1, a1, 0xFFFF
    {0x08864E90u, 0x3402FFFFu}, // ori v0, zero, 0xFFFF
    {0x08864E98u, 0x10A20010u}, // beq a1, v0, (no file)
    {0x08864EA0u, 0x00801821u}, // addu v1, a0, zero
    {0x08863CDCu, 0x27BDFFD0u}, // addiu sp, sp, -48
    {0x08863CE4u, 0x00808021u}, // addu s0, a0, zero
    {0x08863CF0u, 0x313700FFu}, // andi s7, t1, 0xFF
    {0x08863D14u, 0x30B2FFFFu}, // andi s2, a1, 0xFFFF
    {0x08864C54u, 0x27BDFFE0u}, // addiu sp, sp, -32
    {0x08864C6Cu, 0x30B5FFFFu}, // andi s5, a1, 0xFFFF
    {0x08864CFCu, 0x8C620014u}, // lw v0, 0x14(v1)     (the size, through the loader's table)
}};

struct File {
    fs::path path;                                          // a mod's file on disk, or
    std::shared_ptr<const std::vector<std::uint8_t>> bytes; // bytes a mod gave while running
    std::uint32_t size{};
};

struct State {
    std::map<FileId, File> files;
    std::vector<StandIn> stand_ins;
    psprecomp::JitFunction size{};
    psprecomp::JitFunction submit{};
    psprecomp::JitFunction read{};
    bool trace{};
};

State &state() {
    static State s;
    return s;
}

// The game's own function, compiled again when its code changed since.
void run_game(psprecomp::JitFunction &compiled, std::uint32_t address, Runtime &runtime, AllegrexContext &ctx) {
    if (!psprecomp::jit_detached_current(runtime, compiled))
        compiled = psprecomp::jit_compile_detached(runtime, address);
    if (compiled == nullptr) {
        runtime.stop("the game's file loader at " + psprecomp::hex32(address) + " cannot be compiled");
        return;
    }
    compiled(runtime, ctx);
}

// The file a mod serves for the id in a1: one it added, or bytes standing in
// for the disc's.
std::optional<File> served(Runtime &runtime, const AllegrexContext &ctx) {
    const FileId id = ctx.gpr[5] & 0xFFFFu; // a1
    if (const auto found = state().files.find(id); found != state().files.end()) return found->second;
    for (const StandIn &provider : state().stand_ins)
        if (auto bytes = provider(runtime, id); bytes && !bytes->empty())
            return File{{}, bytes, static_cast<std::uint32_t>(bytes->size())};
    return std::nullopt;
}

void size_of(Runtime &runtime, AllegrexContext &ctx) {
    const std::optional<File> file = served(runtime, ctx);
    if (!file) {
        run_game(state().size, kSize, runtime, ctx);
        return;
    }
    ctx.gpr[2] = file->size;
    ctx.pc = ctx.gpr[31];
}

// Copies the first `length` bytes of an added file to the destination in a2.
void copy_file(Runtime &runtime, AllegrexContext &ctx, const File &file, std::uint32_t length) {
    const std::uint32_t destination = ctx.gpr[6]; // a2
    std::vector<std::uint8_t> bytes;
    if (file.bytes) {
        bytes = *file.bytes;
    } else {
        std::ifstream in(file.path, std::ios::binary);
        bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    if (bytes.size() != file.size || !runtime.memory().contains(destination, length)) {
        // A missing file still answers, with zeros, rather than a load that
        // never ends.
        std::cerr << "[mods] file " << psprecomp::hex32(ctx.gpr[5] & 0xFFFFu) << " ("
                  << (file.bytes ? std::string("from memory") : to_utf8(file.path.filename()))
                  << ") cannot be read or does not fit at " << psprecomp::hex32(destination) << "\n";
        bytes.assign(file.size, 0u);
    }
    if (runtime.memory().contains(destination, length)) runtime.memory().copy_in(destination, {bytes.data(), length});
    if (state().trace)
        std::cout << "[mods] file " << psprecomp::hex32(ctx.gpr[5] & 0xFFFFu) << " from a mod: " << length
                  << " bytes to " << psprecomp::hex32(destination) << "\n";
}

void submit(Runtime &runtime, AllegrexContext &ctx) {
    const std::optional<File> file = served(runtime, ctx);
    if (!file) {
        run_game(state().submit, kSubmit, runtime, ctx);
        return;
    }
    copy_file(runtime, ctx, *file, file->size);
    ctx.gpr[2] = 1u;
    ctx.pc = ctx.gpr[31];
}

// The loader's read that waits for the file: it reads the file's sectors
// from DATA.BIN itself, and an added file has none.
void read_now(Runtime &runtime, AllegrexContext &ctx) {
    const std::optional<File> file = served(runtime, ctx);
    if (!file) {
        run_game(state().read, kRead, runtime, ctx);
        return;
    }
    const std::uint32_t asked = ctx.gpr[7]; // a3: 0xFFFFFFFF for the whole file
    const std::uint32_t length = asked == 0xFFFFFFFFu ? file->size : std::min(asked, file->size);
    copy_file(runtime, ctx, *file, length);
    ctx.gpr[2] = length;
    ctx.pc = ctx.gpr[31];
}

} // namespace

namespace {

// Takes the loader's three functions over, once. False when its code is not
// as traced or cannot be compiled.
bool take_over(Runtime &runtime) {
    State &s = state();
    if (s.size != nullptr) return true;
    for (const CodeWord &expected : kSignature) {
        if (!runtime.memory().contains(expected.address, 4u) ||
            runtime.memory().load32(expected.address) != expected.word) {
            std::cerr << "[mods] the game's file loader is not as traced; files mods add are not served\n";
            return false;
        }
    }
    s.trace = std::getenv("MHP3RD_TRACE_MODS") != nullptr;
    // Registered first, so the game's own code is compiled from memory
    // behind them, as a hook's is.
    runtime.register_function(kSize, &size_of, "mhp3rd_added_files_size");
    runtime.register_function(kSubmit, &submit, "mhp3rd_added_files_submit");
    runtime.register_function(kRead, &read_now, "mhp3rd_added_files_read");
    s.size = psprecomp::jit_compile_detached(runtime, kSize);
    s.submit = psprecomp::jit_compile_detached(runtime, kSubmit);
    s.read = psprecomp::jit_compile_detached(runtime, kRead);
    if (s.size == nullptr || s.submit == nullptr || s.read == nullptr) {
        std::cerr << "[mods] the game's file loader cannot be compiled; files mods add are not served\n";
        runtime.unregister_functions(kSize, kSize + 4u);
        runtime.unregister_functions(kSubmit, kSubmit + 4u);
        runtime.unregister_functions(kRead, kRead + 4u);
        s = {};
        return false;
    }
    return true;
}

} // namespace

bool install(Runtime &runtime, const std::map<FileId, Resolution::Source> &files) {
    State &s = state();
    if (files.empty()) return true;
    std::map<FileId, File> found;
    for (const auto &[id, source] : files) {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(source.path, ec);
        if (ec || size > 0x7FFFFFFFu) {
            std::cerr << "[mods] " << source.mod << ": the file it adds as " << psprecomp::hex32(id)
                      << " cannot be read\n";
            continue;
        }
        found[id] = {source.path, nullptr, static_cast<std::uint32_t>(size)};
    }
    if (found.empty() || !take_over(runtime)) return false;
    s.files = std::move(found);
    std::cout << "[mods] " << s.files.size() << " added file(s) served\n";
    return true;
}

bool add(Runtime &runtime, FileId file, std::vector<std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > 0x7FFFFFFFu || state().files.contains(file) || !take_over(runtime))
        return false;
    const auto size = static_cast<std::uint32_t>(bytes.size());
    state().files[file] = {{}, std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)), size};
    return true;
}

bool stand_in(Runtime &runtime, StandIn provider) {
    if (!provider || !take_over(runtime)) return false;
    state().stand_ins.push_back(std::move(provider));
    return true;
}

bool remove(FileId file) {
    const auto found = state().files.find(file);
    if (found == state().files.end() || !found->second.bytes) return false;
    state().files.erase(found);
    return true;
}

bool replace(FileId file, std::vector<std::uint8_t> bytes) {
    const auto found = state().files.find(file);
    if (found == state().files.end() || !found->second.bytes || bytes.empty() || bytes.size() > 0x7FFFFFFFu)
        return false;
    found->second.size = static_cast<std::uint32_t>(bytes.size());
    found->second.bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    return true;
}

FileId free_id() {
    // From the top of the ids mods may add down, away from the ids mods choose
    // themselves (mod.ini's [Add] starts near the disc's last file).
    for (FileId id = kLastFreeId; id >= kFirstFreeId; --id)
        if (!state().files.contains(id)) return id;
    return 0u;
}

bool serves(FileId file) {
    return state().files.contains(file);
}

void reset_for_tests() {
    state() = {};
}

} // namespace mhp3rd::mods::added
