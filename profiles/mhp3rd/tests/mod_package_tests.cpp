// .ymod packages without game data: zip files as packers write them, what
// unpacks and what is refused, and the unpacked folder imported as a mod.
//
//   mhp3rd_mod_package_tests
//   mhp3rd_mod_package_tests --unpack-package <package.ymod> <folder>
//
// The second form unpacks a package made by another tool, to try it by hand.
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mod_import.hpp"
#include "mods/mod_package.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
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
            ("yakumo_mod_package_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(root, ec); // only this test's own scratch folder
    }
};

// A zip file as a packer writes one, for the package tests.
struct ZipEntry {
    std::string name;
    Bytes data;
    Bytes deflated; // the data as a raw DEFLATE stream; empty: stored
    std::uint16_t made_by{20u};
    std::uint32_t attributes{};
    std::uint16_t flags{};
    std::optional<std::uint16_t> method;
    bool wrong_crc{};
};

// Bit by bit, independent of the package reader's table.
std::uint32_t zip_crc(const Bytes &bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

Bytes zip(const std::vector<ZipEntry> &entries) {
    Bytes file, directory;
    const auto put16 = [](Bytes &out, std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8u));
    };
    for (const ZipEntry &entry : entries) {
        const Bytes &packed = entry.deflated.empty() ? entry.data : entry.deflated;
        const std::uint16_t method = entry.method.value_or(entry.deflated.empty() ? 0u : 8u);
        const std::uint32_t crc = zip_crc(entry.data) ^ (entry.wrong_crc ? 1u : 0u);
        const auto offset = static_cast<std::uint32_t>(file.size());
        const auto common = [&](Bytes &out) {
            put16(out, entry.flags);
            put16(out, method);
            append32(out, 0u); // time and date
            append32(out, crc);
            append32(out, static_cast<std::uint32_t>(packed.size()));
            append32(out, static_cast<std::uint32_t>(entry.data.size()));
            put16(out, static_cast<std::uint16_t>(entry.name.size()));
        };
        append32(file, 0x04034B50u);
        put16(file, 20u);
        common(file);
        put16(file, 4u); // an extra field the central directory does not have
        file.insert(file.end(), entry.name.begin(), entry.name.end());
        append32(file, 0u);
        file.insert(file.end(), packed.begin(), packed.end());

        append32(directory, 0x02014B50u);
        put16(directory, entry.made_by);
        put16(directory, 20u);
        common(directory);
        put16(directory, 0u); // extra
        put16(directory, 0u); // comment
        put16(directory, 0u); // disk
        put16(directory, 0u); // internal attributes
        append32(directory, entry.attributes);
        append32(directory, offset);
        directory.insert(directory.end(), entry.name.begin(), entry.name.end());
    }
    const auto at = static_cast<std::uint32_t>(file.size());
    file.insert(file.end(), directory.begin(), directory.end());
    append32(file, 0x06054B50u);
    put16(file, 0u);
    put16(file, 0u);
    put16(file, static_cast<std::uint16_t>(entries.size()));
    put16(file, static_cast<std::uint16_t>(entries.size()));
    append32(file, static_cast<std::uint32_t>(directory.size()));
    append32(file, at);
    put16(file, 0u);
    return file;
}

Bytes text_bytes(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

void test_packages(const fs::path &root) {
    check(is_package("A Mod.YMOD") && is_package("x.ymod") && !is_package("x.zip") && !is_package("ymod"),
        "packages: named .ymod, in any case");

    // Python zlib.compressobj(wbits=-15) of 4096 'a' bytes, as in the
    // runtime's DEFLATE tests.
    const Bytes deflated{
        0xED, 0xC1, 1, 0x0D, 0, 0, 0, 0xC2, 0xA0, 0xAC, 0xEF, 0x5F, 0xC2, 0x1E, 0x0E, 0x28, 0, 0, 0, 0xE0, 0xDD, 0};
    const fs::path packages = root / "packages";
    const Bytes ini = text_bytes("[MOD INFO]\nName=Packed\nType=Yakumo\n[Files]\n0601 = files/armour.pac\n");
    write(packages / "Packed Mod.ymod",
        zip({{"mod.ini", ini}, {"files/", {}}, {"files\\armour.pac", pattern(300, 9)},
            {"big.bin", Bytes(4096u, 'a'), deflated}, {"__MACOSX/._mod.ini", text_bytes("resource fork")},
            {"files/.DS_Store", text_bytes("finder")}}));
    const fs::path staging = root / "unpacked" / "Packed Mod";
    check(unpack_package(packages / "Packed Mod.ymod", staging).empty(), "packages: a package unpacks");
    std::ifstream big(staging / "big.bin", std::ios::binary);
    const Bytes big_bytes((std::istreambuf_iterator<char>(big)), std::istreambuf_iterator<char>());
    check(fs::is_regular_file(staging / "mod.ini") && fs::file_size(staging / "files" / "armour.pac") == 300u &&
            big_bytes == Bytes(4096u, 'a'),
        "packages: stored and deflated files, in folders, with Windows separators");
    check(!fs::exists(staging / "__MACOSX") && !fs::exists(staging / "files" / ".DS_Store"),
        "packages: a packer's clutter is left out");
    const p3::ModFolderFormat format(6043u);
    const ImportCheck found = check_import(staging, format, root / "package mods");
    check(found.mods.size() == 1u && found.mods[0].id == "Packed Mod" && found.mods[0].mod.unusable.empty(),
        "packages: the unpacked folder imports as a mod named after the package");
    check(unpack_package(packages / "Packed Mod.ymod", staging) == "the folder to unpack it into exists already",
        "packages: never into a folder that exists");

    int index = 0;
    const auto refused = [&](const std::string &what, const Bytes &file, const std::string &why,
                             const PackageLimits &limits = {}) {
        const fs::path package = packages / ("refused" + std::to_string(++index) + ".ymod");
        write(package, file);
        const fs::path folder = root / "unpacked" / ("refused" + std::to_string(index));
        const std::string problem = unpack_package(package, folder, limits);
        check(problem.find(why) != std::string::npos && !fs::exists(folder),
            "packages: " + what + " is refused (" + problem + ")");
    };
    const Bytes one = text_bytes("x");
    refused("a name going up", zip({{"../evil.txt", one}}), "outside the package");
    refused("a name going up with backslashes", zip({{"a\\..\\..\\evil.txt", one}}), "outside the package");
    refused("an absolute name", zip({{"/etc/evil", one}}), "outside the package");
    refused("a drive name", zip({{"C:evil", one}}), "outside the package");
    refused("a name that is not text", zip({{std::string("bad\xFF.txt"), one}}), "not readable text");
    refused("a Windows device name", zip({{"files/con.txt", one}}), "cannot be a file name on Windows");
    refused("a numbered device name", zip({{"COM1", one}}), "cannot be a file name on Windows");
    refused("a name ending in a dot", zip({{"a./b.bin", one}}), "cannot be a file name on Windows");
    refused("a name ending in a space", zip({{"b.bin ", one}}), "cannot be a file name on Windows");
    ZipEntry link{"link", text_bytes("/etc/passwd")};
    link.made_by = 0x0314u;
    link.attributes = 0xA1FFu << 16u;
    refused("a symbolic link", zip({link}), "holds a link");
    ZipEntry secret{"mod.ini", one};
    secret.flags = 1u;
    refused("an encrypted entry", zip({secret}), "encrypted");
    ZipEntry bzip{"mod.ini", one};
    bzip.method = 12u;
    refused("another compression method", zip({bzip}), "method Yakumo does not read (12)");
    ZipEntry damaged{"mod.ini", ini};
    damaged.wrong_crc = true;
    refused("a wrong checksum", zip({{"a.bin", one}, damaged}), "checksum");
    refused("a name twice, by case", zip({{"Mod.ini", one}, {"mod.INI", one}}), "twice");
    refused("too many entries", zip({{"a", one}, {"b", one}, {"c", one}}), "more than 2 entries",
        {1u << 20u, 1u << 20u, 2u});
    refused("a file too large", zip({{"big.bin", Bytes(4096u, 'a'), deflated}}), "more than Yakumo accepts",
        {1u << 20u, 1024u, 10u});
    refused("too much in all", zip({{"a", pattern(800, 1)}, {"b", pattern(800, 2)}}), "more than Yakumo accepts",
        {1024u, 1024u, 10u});
    refused("something that is not a zip", pattern(4096, 3), "not a .ymod package");
    refused("an empty file", Bytes{}, "too short");
    Bytes cut = zip({{"mod.ini", ini}});
    cut.resize(cut.size() - 5u);
    refused("a cut package", cut, "not a .ymod package");
    Bytes truncated = zip({{"mod.ini", ini}});
    put32(truncated, truncated.size() - 6u, 2u); // the directory said to start inside the data
    refused("a damaged directory", truncated, "damaged");
    check(unpack_package(packages / "absent.ymod", root / "unpacked" / "absent") == "it cannot be opened",
        "packages: a missing package");
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 4 && std::string(argv[1]) == "--unpack-package") {
        const std::string problem = unpack_package(argv[2], argv[3]);
        std::printf("%s\n", problem.empty() ? "unpacked" : problem.c_str());
        return problem.empty() ? 0 : 1;
    }
    const Scratch scratch;
    test_packages(scratch.root);
    std::printf("%s\n", failures == 0 ? "all package checks passed" : "package checks FAILED");
    return failures == 0 ? 0 : 1;
}
