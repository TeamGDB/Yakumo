#pragma once

#include "mods/mod_library.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace psprecomp {
class Runtime;
}

// Quests mods add (mod.ini's [Quests]): each is encrypted with the game's own
// keys, read from the executable in memory (game/quest_file.hpp), and served
// as a new file (mhp3rd_mod_format.hpp's quest_file()). The game's code that
// turns a quest id into its file (0x088C1770) has no branch for ids from
// 60000: every such id reaches the instruction at 0x088C18B8 with no file and
// the id in t1. A hook there gives it the mod's file in s0, and the quest
// loads, decrypts and plays like the game's own (docs/QUESTS.md).
//
// Installed at start when a mod adds a quest, and only on the executable the
// code was traced in.
namespace mhp3rd::mods::quests {

// A quest a mod added, as the developer tools list it.
struct Quest {
    std::uint32_t id{};
    std::string name;
    std::uint32_t fee{};
    std::uint32_t frames{};
    std::uint16_t stars{};
    std::string mod;
};

// Encrypts `quests` into `cache` and adds each to `files` under its file
// id, then installs the hook. False, with the reason logged, when the keys
// or the traced code are not there; `files` is then left as it was.
bool install(psprecomp::Runtime &runtime, const std::map<FileId, Resolution::Source> &quests,
    std::map<FileId, Resolution::Source> &files, const std::filesystem::path &cache);

// The quests being served, by id.
[[nodiscard]] const std::vector<Quest> &served();
[[nodiscard]] const Quest *find(std::uint32_t id);

// Forgets the quests and the hook's table, so tests can install again.
void reset_for_tests();

} // namespace mhp3rd::mods::quests
