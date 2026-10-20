#pragma once

#include "mods/file_overlay.hpp"
#include "mods/mod_library.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The mod folders this game's community already uses, so that mods made for
// mhp3reload and its mod manager work unchanged. Implemented from their
// published documentation and from the files themselves, not from their code
// (see docs/SOURCE_PROVENANCE.md):
//
// - A mod is a folder with a mod.ini whose [MOD INFO] section names it, its
//   Type, its Files, and for file and patch mods the Target file ids.
// - A file id is a DATA.BIN entry index in four upper-case hex digits.
// - A folder without a mod.ini that holds files named by file id (0601) and
//   patches named by file id and P (0601P), as mhp3reload's own files folder
//   does, is taken as one mod.
//
// Game-specific: the machinery that uses this is in mod_library.hpp.
namespace mhp3rd::mods::p3rd {

class ModFolderFormat final : public ModFormat {
public:
    // `entries`: how many files DATA.BIN has; targets beyond it are refused.
    // 0 when not known yet.
    explicit ModFolderFormat(std::uint32_t entries = 0u) : entries_(entries) {}
    void set_entries(std::uint32_t entries) noexcept { entries_ = entries; }

    [[nodiscard]] std::optional<Mod> read(const std::filesystem::path &folder) const override;
    [[nodiscard]] std::string file_name(FileId file) const override;
    [[nodiscard]] std::optional<FileId> parse_file(const std::string &text) const override;
    // file_replacer: the loader's own file replacement, which Yakumo does.
    [[nodiscard]] bool built_in(const std::string &dependency) const override;

private:
    std::uint32_t entries_;
};

// Quests mods add (mod.ini's [Quests], ids from game::kFirstModQuest) are
// served as new files: quest 62000 + n as file 6000 + n (hex). [Add] keeps
// out of that range, so quest files never meet a mod's own.
inline constexpr FileId kFirstQuestFile = 0x6000u;
inline constexpr FileId kQuestFiles = 0x10000u - 62000u;
// A file of a mod by its path in the mod's folder (either slash, any case),
// or nothing for a path that leaves the folder or a file that is not there.
[[nodiscard]] std::optional<std::filesystem::path> mod_file(
    const std::filesystem::path &folder, const std::string &relative);

[[nodiscard]] std::optional<FileId> quest_file(std::uint32_t quest);
[[nodiscard]] std::optional<std::uint32_t> quest_of_file(FileId file);

// Guest RAM, with an address given through a cached or uncached mirror folded
// onto it: the address `length` bytes at `address` start at, or nothing when
// they are not all in RAM.
[[nodiscard]] std::optional<std::uint32_t> ram_range(std::uint32_t address, std::size_t length);

// One block of a patch or code file: `bytes` for guest address `address`.
// `run`: the top bit of its length, "run it as it loads".
struct Block {
    std::uint32_t address{};
    std::vector<std::uint8_t> bytes;
    bool run{};
    std::vector<std::uint8_t> expect; // from an Edit: written only over these bytes
};
struct Blocks {
    std::vector<Block> blocks;
    std::vector<std::string> problems; // why reading stopped early
};

// Where a code block that names no address goes: `id` is the four
// characters the block names itself by, `size` its length. The address, or
// nothing when there is no memory for it.
using Place = std::function<std::optional<std::uint32_t>(const std::string &id, std::uint32_t size)>;

// The blocks of a patch or code file. Published mods use two layouts:
//
// - Plain: blocks of an address, a length (top bit: run it as it loads) and
//   that many bytes, up to the end address FFFFFFFF or the end of the data.
// - Typed: a 32-bit type before each block, optionally after a four-character
//   version ("0.01"). 0 is a plain block; 1 a main block, code with a length
//   and an id but no address, which `place` puts somewhere; 2 a hook, a j or
//   jal written at an address to an offset into the last main block, with a
//   nop after it when its filler byte is set; 3 an init block, code placed
//   like a main block that runs as it loads; -1 ends the file.
//
// Both come back as plain blocks: a placed block at its address, a hook as
// the instruction(s) it writes.
[[nodiscard]] Blocks read_blocks(std::span<const std::uint8_t> data, const Place &place = {});
[[nodiscard]] Blocks read_blocks(const std::filesystem::path &file, const Place &place = {});
// The blocks of a patch or code given in mod.ini: its file, or its edits.
[[nodiscard]] Blocks read_blocks(const Resolution::Source &source, const Place &place = {});

// "expected 27 BD FF F0, found 00 00 00 00": why an edit was not written, or
// nothing when `found` is what it expects.
[[nodiscard]] std::optional<std::string> mismatch(
    std::span<const std::uint8_t> expect, std::span<const std::uint8_t> found);

// Applies one patch file to a file's bytes. A patch is a list of blocks, each
// a guest address, a length and that many bytes, ended by the address
// FFFFFFFF (the "last 8 bytes FFFFFFFF00000000" of the format, which the
// manager strips when it installs a patch; both forms are read).
//
// - A block for an address in a code overlay's data (the overlay's header
//   gives where it loads) changes those bytes of the file, which the game
//   copies to that address unchanged.
// - A block for the overlay's header or code, or for other guest memory, is
//   written there once the overlay has loaded (FileContent::after_load): the
//   overlay's recompiled code is installed from the bytes it was made from
//   first, and only the units a block changes then leave native code.
// - A block whose address is smaller than the file is taken as an offset
//   into the file, for files that load to no fixed address.
// - In a patch for a file that is not a code overlay, a block for guest
//   memory is written there once the game has read the whole file: the
//   loader's patches are code loaded along with a file.
// - A block marked to run as it loads (the top bit of its length) is
//   written like the others and then called, once the overlay has loaded
//   (PatchOutcome::run). In a file that loads to no fixed address it is
//   skipped and reported.
[[nodiscard]] PatchOutcome apply_patch(std::vector<std::uint8_t> &bytes, const std::filesystem::path &patch);
// The same for blocks already read; an edit's expected bytes are checked
// where it changes the file, and passed on where it writes memory.
[[nodiscard]] PatchOutcome apply_patch(std::vector<std::uint8_t> &bytes, const Blocks &patch);

// A code overlay's header at the start of a file: where it loads, and its
// image size (header, code and data). Nothing when the file is not one.
struct OverlayImage {
    std::uint32_t load{};
    std::uint32_t code_end{}; // guest address after the code
    std::uint32_t size{};
};
[[nodiscard]] std::optional<OverlayImage> overlay_image(const std::vector<std::uint8_t> &bytes);

} // namespace mhp3rd::mods::p3rd
