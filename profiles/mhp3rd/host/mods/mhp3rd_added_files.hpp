#pragma once

#include "mods/mod_library.hpp"

#include <map>
#include <vector>

namespace psprecomp {
class Runtime;
}

// Files mods add to DATA.BIN under new ids (mod.ini's [Add]), from 6043 up.
//
// The game's loader keeps every table it indexes by file id (block
// addresses, cached slots, preloads) at a fixed size for the disc's 6043
// entries, in one object at 0x08B3F040, so new entries cannot be appended to
// the archive. Instead the two places a whole-file load passes through are
// taken over for the added ids only: the loader's size lookup (0x08864E8C,
// which the game also asks before it allocates) answers the added file's
// size, and the request submit (0x08863CDC) copies the file to its
// destination at once and queues nothing, so the game's own wait for the
// queue sees it done. Every other id runs the game's code unchanged.
//
// Installed at start when a mod adds a file, and only on the executable the
// two functions were traced in (docs/DATA_BIN.md).
namespace mhp3rd::mods::added {

// Takes the two functions over for `files`. False, with the reason logged,
// when the executable's code is not as traced or a file cannot be read.
bool install(psprecomp::Runtime &runtime, const std::map<FileId, Resolution::Source> &files);

// Adds a file a mod made while running (bytes, copied), under `file`. False
// when the id is taken, the file is empty, or the loader cannot be taken over.
bool add(psprecomp::Runtime &runtime, FileId file, std::vector<std::uint8_t> bytes);

// The ids add() hands out when a mod does not choose one: the top of the range
// mods may add to, below the ids an id-less number can reach (0xFFFF means no
// file) and above the quests' 0x6000 to 0x759F.
inline constexpr FileId kFirstFreeId = 0xC000u;
inline constexpr FileId kLastFreeId = 0xFFFEu;

// The highest free id in that range, or 0 when every one is taken.
[[nodiscard]] FileId free_id();

// Whether `file` is an added file being served; for tests and traces.
[[nodiscard]] bool serves(FileId file);

// Forgets the files, so tests can install again.
void reset_for_tests();

} // namespace mhp3rd::mods::added
