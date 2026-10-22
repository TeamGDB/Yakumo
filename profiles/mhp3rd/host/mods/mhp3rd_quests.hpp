#pragma once

#include "mods/mod_library.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace psprecomp {
class Runtime;
}

// Quests mods add (mod.ini's [Quests]): each is encrypted with the game's own
// keys, read from the executable in memory (game/quest_file.hpp), and served
// as a new file (mhp3rd_mod_format.hpp's quest_file()), under an id from
// 62000 (docs/QUESTS.md). The game takes ids above 59999 for event quests:
//
// - At the counter, Event Quest offers the event quests the game includes,
//   loaded from their files into the download slots by 0x0888F9F4 when there
//   is no save of downloaded quests. Mods' ids join the game's list
//   (game table "quest.events"), and an instruction hook where 0x0888F9F4
//   has worked out the file (0x0888FA4C) gives it the mod's in s3. The game
//   decrypts, lists and plays them like its own.
// - Its code that turns a quest id into its file (0x088C1770) has no branch
//   for them: such an id reaches the instruction at 0x088C18B8 with no file
//   and the id in t1. A hook there gives it the mod's file in s0, for the
//   developer tools' quest start.
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

// Serves a quest a mod made while it starts (yakumo.quests): encrypted and
// added as a file from memory, at the counter and for the developer tools as
// mod.ini's quests are. False, with the reason in `why`, when it is not a
// quest under a mod's id, its id is served already, or the keys or the traced
// code are not there.
bool add(
    psprecomp::Runtime &runtime, std::vector<std::uint8_t> payload, const std::string &mod, std::string *why = nullptr);

// Builds quest `id` again once the game's text is loaded, which some of its
// parts are named by (reward items): `build` is called once lobby_task loads
// and its payload, encrypted, replaces the one served. A build that fails
// leaves the first payload served, with the reason in the log.
using Rebuild = std::function<std::optional<std::vector<std::uint8_t>>(psprecomp::Runtime &, std::string *why)>;
void rebuild_later(std::uint32_t id, Rebuild build);

// Writes mods' quests into the event quest list once lobby_task, where it
// is, has loaded (after tables::overlay_loaded() moved it); first builds again
// the quests rebuild_later() named.
void overlay_loaded(psprecomp::Runtime &runtime);

// The quests being served, by id.
[[nodiscard]] const std::vector<Quest> &served();
[[nodiscard]] const Quest *find(std::uint32_t id);

// Forgets the quests and the hook's table, so tests can install again.
void reset_for_tests();

} // namespace mhp3rd::mods::quests
