// Code mods without game data: the loader's block files, typed blocks, and
// writing them into memory before the game's first instruction, with the
// generated code they replace retired.
#include "mods/mhp3rd_code_mods.hpp"
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mod_ini.hpp"
#include "mods/mod_library.hpp"

#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace mhp3rd::mods;
namespace fs = std::filesystem;
namespace p3 = mhp3rd::mods::p3rd;
using Bytes = std::vector<std::uint8_t>;

namespace {

int failures = 0;

void check(bool condition, const std::string &what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) ++failures;
}

void write(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

void write(const fs::path &path, const Bytes &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void put32(Bytes &bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
}

void append32(Bytes &bytes, std::uint32_t value) {
    bytes.resize(bytes.size() + 4u);
    put32(bytes, bytes.size() - 4u, value);
}

Bytes pattern(std::size_t size, std::uint32_t seed) {
    std::mt19937 random(seed);
    Bytes bytes(size);
    for (std::uint8_t &b : bytes) b = static_cast<std::uint8_t>(random());
    return bytes;
}

struct Scratch {
    fs::path root;
    Scratch() {
        root = fs::temp_directory_path() /
            ("yakumo_code_mods_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root, ec); // only this test's own scratch folder
    }
};

// A mod folder with a mod.ini in the manager's format.
void make_mod(const fs::path &folder, const std::string &info, const std::vector<std::string> &files = {}) {
    write(folder / "mod.ini", "[MOD INFO]\r\n" + info);
    for (const std::string &file : files) write(folder / file, pattern(64, 7));
}

// A block file in the loader's format.
struct BlockFile {
    Bytes bytes;
    BlockFile &block(std::uint32_t address, const Bytes &payload, bool run = false) {
        append32(bytes, address);
        append32(bytes, static_cast<std::uint32_t>(payload.size()) | (run ? 0x80000000u : 0u));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return *this;
    }
    BlockFile &end() {
        append32(bytes, 0xFFFFFFFFu);
        append32(bytes, 0u);
        return *this;
    }
};

void unit_a(psprecomp::Runtime &, psprecomp::AllegrexContext &cpu) {
    cpu.pc += 4u;
}
void unit_b(psprecomp::Runtime &, psprecomp::AllegrexContext &cpu) {
    cpu.pc += 4u;
}
void host_function(psprecomp::Runtime &, psprecomp::AllegrexContext &cpu) {
    cpu.pc += 4u;
}

void test_code(const fs::path &root) {
    // The blocks of a code file.
    BlockFile file;
    file.block(0x08A00000u, {1, 2, 3}).block(0x48A00010u, {4}, true).end().block(0x08A00020u, {5});
    p3::Blocks read = p3::read_blocks(file.bytes);
    check(read.blocks.size() == 2u && read.problems.empty() && read.blocks[0].bytes == Bytes{1, 2, 3} &&
            read.blocks[1].run && read.blocks[1].address == 0x48A00010u,
        "code: blocks up to the end marker, with the run bit");
    BlockFile truncated;
    truncated.block(0x08A00000u, {1});
    truncated.bytes.resize(truncated.bytes.size() + 8u, 0u);
    put32(truncated.bytes, truncated.bytes.size() - 8u, 0x08A00004u);
    put32(truncated.bytes, truncated.bytes.size() - 4u, 100u);
    read = p3::read_blocks(truncated.bytes);
    check(read.blocks.size() == 1u && read.problems.size() == 1u, "code: a block past the end of the file is reported");
    check(p3::read_blocks(root / "code" / "absent.bin").problems.size() == 1u, "code: a missing file is reported");
    check(p3::ram_range(0x48A00000u, 4u) == 0x08A00000u && !p3::ram_range(0x0BFFFFFEu, 4u) &&
            !p3::ram_range(0x00010000u, 4u),
        "code: guest RAM through a mirror, and its end");

    // Which code the enabled mods write, and in what order.
    const p3::ModFolderFormat format(6043u);
    const fs::path mods = root / "code" / "mods";
    make_mod(
        mods / "a_core", "Name=\"Core\"\nType=\"Code\"\nVersion=\"HD\"\nFiles=\"a.bin\"\nPriority=\"0\"\n", {"a.bin"});
    make_mod(mods / "b_low", "Name=\"Low\"\nType=\"Code\"\nVersion=\"HD\"\nFiles=\"b.bin\"\n", {"b.bin"});
    make_mod(mods / "c_high", "Name=\"High\"\nType=\"Code\"\nVersion=\"HD\"\nFiles=\"c.bin\"\n", {"c.bin"});
    ModLibrary library(format);
    library.scan(mods);
    for (const std::string id : {"a_core", "b_low", "c_high"}) library.set_enabled(id, true);
    Resolution resolution = library.resolve();
    check(resolution.code.size() == 3u && resolution.code[0].mod == "a_core" && resolution.code[1].mod == "b_low" &&
            resolution.code[2].mod == "c_high" && !resolution.empty(),
        "code: priority 0 first, then the higher-ranked mod last, so it wins");
    Resolution other = resolution;
    std::swap(other.code[1], other.code[2]);
    check(!resolution.same_files(other), "code: a different order is a different set of mods");
    library.set_enabled("b_low", false);
    check(library.resolve().code.size() == 2u, "code: a mod turned off writes nothing");
    library.set_master(false);
    check(library.resolve().code.empty(), "code: nothing with every mod off");

    // Written into memory: the generated code under it is dropped, and the
    // port's own functions there are reported.
    psprecomp::Runtime runtime(64u * 1024u * 1024u);
    constexpr std::uint32_t kCode = 0x08804000u;
    runtime.register_generated_unit(0, kCode, 0x4000, unit_a);
    runtime.register_generated_unit(1, kCode + 0x4000u, 0x4000, unit_b);
    for (std::uint32_t at = 0; at < 0x4000u; at += 4u) {
        runtime.register_function(kCode + at, unit_a, "recomp_unit_0000");
        runtime.register_function(kCode + 0x4000u + at, unit_b, "recomp_unit_0001");
    }
    runtime.register_function(kCode + 0x4100u, host_function, "mhp3rd_host_function");
    BlockFile first;
    first
        .block(kCode + 0x10u, {0x01, 0x00, 0x02, 0x24}) // in unit 0
        .block(0x09000000u, {0xAA, 0xBB})               // data, no code
        .block(0x09000001u, {0xCC})                     // later blocks win
        .block(0x08A00000u, {1}, true)                  // to run as it loads
        .block(0x10000000u, {1})                        // outside memory
        .block(0x08800004u, {1, 2, 3, 4})               // memory Yakumo keeps for itself
        .end();
    BlockFile second;
    second.block(kCode + 0x40FCu, Bytes(8u, 0u)); // unit 1, up to and into the port's own function
    write(root / "code" / "first.bin", first.bytes);
    write(root / "code" / "second.bin", second.bytes);
    const auto code_file = [](const std::string &who, const fs::path &path) {
        return p3::CodeFile{who, Resolution::Source{who, path, {}}};
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> reserved;
    const p3::Reserve reserve = [&reserved](std::uint32_t address, std::uint32_t size) {
        reserved.emplace_back(address, size);
        if (address == 0x08800004u) return p3::Reservation{false, "is Yakumo's"};
        return p3::Reservation{true, address == 0x09000001u ? "is busy" : ""};
    };
    const p3::CodeReport report = p3::write_code(runtime,
        {code_file("First", root / "code" / "first.bin"), code_file("Second", root / "code" / "second.bin"),
            code_file("Gone", root / "x.bin")},
        reserve);
    check(runtime.memory().load32(kCode + 0x10u) == 0x24020001u && runtime.memory().load8(0x09000000u) == 0xAAu &&
            runtime.memory().load8(0x09000001u) == 0xCCu,
        "code: blocks are written to memory in order");
    check(report.blocks == 5u && report.bytes == 4u + 2u + 1u + 1u + 8u && report.units == 2u,
        "code: each unit written into is dropped once");
    check(report.run == std::vector<std::uint32_t>{0x08A00000u} && runtime.memory().load8(0x08A00000u) == 1u,
        "code: a block to run as it loads is written and listed to be called");
    check(runtime.memory().load32(0x08800004u) == 0u, "code: memory a reservation refuses is not written");
    check(
        !runtime.has_function(kCode) && !runtime.has_function(kCode + 0x4000u) && runtime.has_function(kCode + 0x4100u),
        "code: dropped units run interpreted; the port's own function stays");
    const auto mentions = [&report](const std::string &who, const std::string &text) {
        return std::count_if(report.problems.begin(), report.problems.end(),
            [&](const std::string &line) { return line.starts_with(who) && line.find(text) != std::string::npos; });
    };
    check(mentions("First", "is Yakumo's") == 1 && mentions("First", "outside") == 1 &&
            mentions("Second", "replaces with its own") == 1 && mentions("Gone", "cannot read") == 1 &&
            mentions("First", "0x09000001 is busy") == 1 && report.problems.size() == 5u,
        "code: skipped blocks, a missing file, a change the port overrides and busy memory are reported");
    check(reserved.size() == 6u && reserved[1] == std::pair<std::uint32_t, std::uint32_t>{0x09000000u, 2u} &&
            reserved[5] == std::pair<std::uint32_t, std::uint32_t>{kCode + 0x40FCu, 8u},
        "code: the memory of every block written is reserved");
}

// The loader's typed files: a type before each block, main blocks placed
// by the caller, hooks pointing into them.
void test_typed_blocks() {
    Bytes typed;
    const auto u32 = [&typed](std::uint32_t v) { append32(typed, v); };
    for (char c : std::string("0.01")) typed.push_back(static_cast<std::uint8_t>(c)); // version
    u32(1u);                                                                          // main block
    u32(0x80000008u);                                                                 // 8 bytes, run it
    for (char c : std::string("TEST")) typed.push_back(static_cast<std::uint8_t>(c));
    u32(0x03E00008u);
    u32(0u);
    u32(2u); // hook: jal main+4 at 0x08900000, with a nop after it
    u32(0x08900000u);
    typed.insert(typed.end(), {0x04, 0x00, 0x0C, 0x01});
    u32(2u); // hook: j main at 0x08900010, no nop
    u32(0x08900010u);
    typed.insert(typed.end(), {0x00, 0x00, 0x08, 0x00});
    u32(0u); // plain block
    u32(0x0A000000u);
    u32(2u);
    typed.insert(typed.end(), {0xAB, 0xCD});
    u32(3u); // init block
    u32(4u);
    u32(0x03E00008u);
    u32(0xFFFFFFFFu); // end
    u32(0x12345678u); // after the end: ignored
    std::vector<std::pair<std::string, std::uint32_t>> asked;
    std::uint32_t next = 0x0B000000u;
    const p3::Place place = [&](const std::string &id, std::uint32_t size) -> std::optional<std::uint32_t> {
        asked.emplace_back(id, size);
        const std::uint32_t at = next;
        next += 0x100u;
        return at;
    };
    p3::Blocks read = p3::read_blocks(typed, place);
    check(read.problems.empty() && read.blocks.size() == 5u, "typed: every block, past the version and to the end");
    check(
        asked.size() == 2u && asked[0] == std::pair<std::string, std::uint32_t>{"TEST", 8u} && asked[1].first == "init",
        "typed: main and init blocks are placed by id and size");
    check(read.blocks.size() == 5u && read.blocks[0].address == 0x0B000000u && read.blocks[0].run &&
            read.blocks[0].bytes.size() == 8u,
        "typed: a main block goes where it was placed, keeping its run bit");
    // jal 0x0B000004 = 0x0C000000 | (0x0B000004 >> 2)
    check(read.blocks.size() == 5u && read.blocks[1].address == 0x08900000u &&
            read.blocks[1].bytes == Bytes{0x01, 0x00, 0xC0, 0x0E, 0, 0, 0, 0},
        "typed: a hook is a jal into the main block, with a nop after it");
    check(read.blocks.size() == 5u && read.blocks[2].bytes == Bytes{0x00, 0x00, 0xC0, 0x0A},
        "typed: a hook without filler is a lone j");
    check(read.blocks.size() == 5u && read.blocks[3].address == 0x0A000000u &&
            read.blocks[3].bytes == Bytes{0xAB, 0xCD} && read.blocks[4].address == 0x0B000100u && read.blocks[4].run,
        "typed: plain blocks keep their address; an init block is placed and runs");

    read = p3::read_blocks(typed);
    check(read.problems.size() == 4u && read.blocks.size() == 1u && read.blocks[0].address == 0x0A000000u,
        "typed: without memory for code, its blocks and the hooks into them are reported");

    // Not typed after all: an unknown type falls back to the plain layout.
    // Here a plain block for file offset 1 would be a main block running past
    // the end of the data.
    Bytes plain;
    append32(plain, 1u);
    append32(plain, 2u);
    plain.insert(plain.end(), {0xAB, 0xCD});
    read = p3::read_blocks(plain, place);
    check(read.problems.empty() && read.blocks.size() == 1u && read.blocks[0].address == 1u &&
            read.blocks[0].bytes == Bytes{0xAB, 0xCD},
        "typed: data that does not read as typed is read as plain");
}

} // namespace

int main() {
    const Scratch scratch;
    test_code(scratch.root);
    test_typed_blocks();
    std::printf("%s\n", failures == 0 ? "all code mod checks passed" : "code mod checks FAILED");
    return failures == 0 ? 0 : 1;
}
