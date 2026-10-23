#pragma once

// A one-shot diagnostic dump for the lobby report. The offsets in the report
// were traced on an old save; this writes the live character block, the four
// hunter records, the hall board windows and every place the hunter's name
// appears, so the right fields can be traced from a real session.
//
// Off by default. Set MHP3RD_LOBBY_DUMP to a directory (or "1" for a logs
// folder in the working directory) and enter a gathering hall. Up to three
// dumps are written per hall, ten seconds apart, so the board can be captured
// after a quest is posted. MHP3RD_LOBBY_DUMP_TEXT adds a text to search for
// (default "Zinogre"); MHP3RD_LOBBY_DUMP_FULL also writes the whole 64 MiB of
// guest RAM on the second and third dump. MHP3RD_LOBBY_DUMP_ANY also dumps
// outside a hall, which captures the hall-creation screen.

#include "lobby/report.hpp"

namespace mhp3rd::game {
class Ram;
}

namespace mhp3rd::lobby {

void maybe_dump(const game::Ram &ram, const Snapshot &snapshot);

} // namespace mhp3rd::lobby
