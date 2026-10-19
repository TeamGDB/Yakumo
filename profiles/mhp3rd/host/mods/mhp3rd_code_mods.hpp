#pragma once

#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mod_library.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace psprecomp {
class Runtime;
}

// Mods that change the game's code in memory. The game runs as code
// recompiled ahead of time, which never reads its own instructions from guest
// memory, so a change there would do nothing. Every generated unit (16 KiB of
// the game's code) a mod writes into is dropped instead, and the interpreter
// runs those addresses from memory, the mod's instructions included. The rest
// of the game stays native.
//
// Game-specific: the code files are the community loader's block files
// (mhp3rd_mod_format.hpp).
namespace mhp3rd::mods::p3rd {

struct CodeFile {
    std::string who;           // how problems name it: the mod and the file
    Resolution::Source source; // a block file, or edits from mod.ini
};

struct CodeReport {
    std::size_t blocks{}; // blocks written
    std::size_t bytes{};
    std::size_t units{};               // generated units dropped
    std::vector<std::uint32_t> run;    // blocks to call as they load, in order
    std::vector<std::string> problems; // blocks skipped or without effect, and why
};

// Drops the generated code under `length` bytes at `address`, which were just
// changed in memory. A problem is added when part of the range is code the
// port replaces with its own (a host function or an import), which the
// change cannot reach. Returns the units dropped.
std::size_t retire_code(psprecomp::Runtime &runtime, std::uint32_t address, std::size_t length, const std::string &who,
    std::vector<std::string> &problems);

// Keeps the game from allocating memory a mod's code is about to be written
// to. `write` false keeps the block out of memory entirely (memory Yakumo
// itself needs); `problem` says why memory could not be reserved.
struct Reservation {
    bool write{true};
    std::string problem;
};
using Reserve = std::function<Reservation(std::uint32_t address, std::uint32_t size)>;

// Writes the blocks of `files` into guest memory in order, the last winning
// where two overlap, drops the generated code under them, and reserves the
// memory with `reserve` when it is given. `place` puts code blocks that name
// no address. Blocks that run as they load are written and listed in
// CodeReport::run for the caller to call; edits whose expected bytes are not
// there are skipped and reported. `trace` logs every block written.
[[nodiscard]] CodeReport write_code(psprecomp::Runtime &runtime, const std::vector<CodeFile> &files,
    const Reserve &reserve = {}, const Place &place = {}, bool trace = false);

} // namespace mhp3rd::mods::p3rd
