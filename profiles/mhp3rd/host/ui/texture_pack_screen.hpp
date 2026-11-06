#pragma once

#include <string>

// The Video page's texture pack import: a folder chosen with the file browser
// (or dropped on the window), the pack found in it checked and shown beside
// the installed one, then copied into the data folder on a thread of its own,
// or used where it is (see gpu/texture_pack_import.hpp).
namespace mhp3rd::ui {

// The rows under Texture pack: Import texture pack…, Open the textures
// folder, and, for a pack used in place, a row that stops using it.
void texture_pack_rows();
// System > Game Data: remove the installed copy after confirmation.
void texture_pack_data_rows();

// Whether the import screen is open.
[[nodiscard]] bool texture_pack_screen_open();

// Whether a copy is running or waiting to be put in place; the menu stays
// open until it ends.
[[nodiscard]] bool texture_pack_import_busy();

// Moves a finished copy into place. The menu calls it every frame, whichever
// page it shows.
void texture_pack_import_tick();

// Draws the open screen in place of the page. `back`: the back button was
// pressed this frame. True while the screen is open.
bool texture_pack_screen(bool back);

// Current nested screen, empty at the owning tab.
[[nodiscard]] std::string texture_pack_screen_path();

} // namespace mhp3rd::ui
