#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// .ymod packages: a mod folder, or a folder of several, packed as one .zip
// file under another name, so a mod travels as one file and is installed by
// dropping it on the window. The zip's top holds the mod's mod.ini, or the
// mod folders; mod_import.hpp takes it from there once it is unpacked.
//
// A package is read as untrusted input: only stored and deflated entries,
// no encryption, links or Zip64, names that stay inside the folder, every
// CRC checked, and limits on the number of entries and their sizes.
//
// Game-independent.
namespace mhp3rd::mods {

struct PackageLimits {
    std::uint64_t bytes{1ULL << 30};        // all files, unpacked
    std::uint64_t file_bytes{512ULL << 20}; // one file, unpacked
    std::size_t entries{20000};
};

// Whether `path` is named as a package (.ymod, in any case).
[[nodiscard]] bool is_package(const std::filesystem::path &path);

// Unpacks `package` into `folder`, which must not exist yet. Returns an empty
// string when it worked, or why not, for the player; then nothing of
// `folder` is left behind.
[[nodiscard]] std::string unpack_package(
    const std::filesystem::path &package, const std::filesystem::path &folder, const PackageLimits &limits = {});

} // namespace mhp3rd::mods
