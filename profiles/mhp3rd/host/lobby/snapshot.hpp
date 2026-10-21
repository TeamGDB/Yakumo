#pragma once

// The hunter's own data, read from guest memory for the lobby report. Pure
// functions over a game::Ram, so the unit tests run them on a buffer.

#include "lobby/report.hpp"

namespace mhp3rd::game {
class Ram;
}

namespace mhp3rd::lobby {

// Fills `snapshot` with the name, sex, worn armor and carried weapon of the
// loaded hunter, with the game's own names when its text block is loaded.
// Fields the game does not have loaded yet are left empty.
void read_hunter(const game::Ram &ram, Snapshot &snapshot);

} // namespace mhp3rd::lobby
