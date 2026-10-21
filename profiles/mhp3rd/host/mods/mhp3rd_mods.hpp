#pragma once

#include "mods/mod_session.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psprecomp {
class Runtime;
}

namespace mhp3rd {
class IsoImage;
}

// Mods for this game: the mods folder in the per-user data directory, read in
// the community's format (mhp3rd_mod_format.hpp), served to the game through
// its DATA.BIN reads (mhp3rd_data_bin.hpp).
//
// With no mods folder, or no mod on, none of this changes a byte the game
// reads: the file I/O asks serving() and reads the disc as before.
//
//   MHP3RD_MODS_DIR    the mods folder, instead of mods/ in the data directory
//   MHP3RD_NO_MODS=1   no mod applies this run; the menu still lists them
//   MHP3RD_TRACE_MODS  logs what the mods change and every read they serve
namespace mhp3rd::mods {

// The disc image, once it is open, before the game runs. Reads the mods
// folder and the choices in mods.ini.
void attach_disc(IsoImage *disc);

// Where DATA.BIN is on the disc image, in bytes. Nothing without a disc.
struct DiscRange {
    std::uint64_t offset{};
    std::uint64_t size{};
};
[[nodiscard]] std::optional<DiscRange> data_bin_on_disc();

// Whether the mods change what the game reads from DATA.BIN now. Cheap.
[[nodiscard]] bool serving();
// DATA.BIN's size as the game sees it: larger when a mod's file grew it.
[[nodiscard]] std::uint64_t data_bin_size();
// Reads DATA.BIN as the game sees it, at an offset into the archive.
std::size_t read_data_bin(std::uint64_t offset, std::span<std::uint8_t> out);

// The active archive entry that holds byte `offset` of DATA.BIN, and how far
// into it that is. Directory bytes, padding and out-of-range offsets return
// nothing. Pending layout changes do not apply until activation. The text
// translation uses it to know which block a read brought in.
struct EntryAt {
    std::uint32_t entry{};
    std::uint64_t into{}; // bytes from the entry's start
    std::uint64_t size{}; // the entry's exact size
};
[[nodiscard]] std::optional<EntryAt> entry_at_offset(std::uint64_t offset);

// Read one complete encoded entry from the active archive without changing
// guest load tracking. The caller supplies a memory bound before allocation.
struct ArchiveEntry {
    std::uint64_t offset{};
    std::vector<std::uint8_t> bytes;
};
[[nodiscard]] std::optional<ArchiveEntry> read_archive_entry(std::uint32_t entry, std::size_t max_bytes);

// The game flushed its instruction cache, which it does right after copying a
// code overlay into place: the point where a code overlay has finished
// loading. Writes the patches that go to memory outside a loaded overlay, and
// returns the guest code the mods want called now, in order: blocks marked to
// run as they load, of the overlay's patches and, at the first flush, of the
// code mods written at start. The caller calls them on the game's thread.
[[nodiscard]] std::vector<std::uint32_t> code_loaded(psprecomp::Runtime &runtime);

// The code mods, written into guest memory once the executable is loaded and
// before its first instruction runs. The game's recompiled code cannot see a
// change to its instructions in memory, so every generated unit (16 KiB of
// code) a block writes into is dropped and runs in the interpreter from then
// on: slower, but it runs the mod's instructions. Blocks marked to run as
// they load run at the game's first code load (code_loaded). Code mods apply
// at start only; turning one on or off waits for a restart.
void write_code(psprecomp::Runtime &runtime);

// Starts the native mods of the mods that are on (native_mods.hpp), after
// write_code() and before the game runs.
void start_native(psprecomp::Runtime &runtime);

// The game's own bytes of a DATA.BIN file, decrypted. Nothing without a disc.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> original_file(FileId file);

// The session the menu manages; null when there is no disc.
[[nodiscard]] ModSession *session();
[[nodiscard]] std::filesystem::path mods_directory();
// "Applies at the next load" or "Restart to apply": what the last change needs.
[[nodiscard]] std::string pending_note();
// Problems met while serving a mod's file this run, newest last.
[[nodiscard]] const std::vector<std::string> &problems();

} // namespace mhp3rd::mods
