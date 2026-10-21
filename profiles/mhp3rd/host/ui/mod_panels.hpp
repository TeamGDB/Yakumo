#pragma once

#include <string>

// What native and script mods draw through "yakumo.ui" (sdk/include/yakumo/
// mod.h): panels on a mod's page in the Mods menu, drawn with the menu's own
// rows so a gamepad reaches them, and overlays over the running game, each in
// a small window that takes no input.
namespace mhp3rd::ui {

// The panels of the mod `mod_id`, under a heading each. Inside a menu frame.
void draw_mod_panels(const std::string &mod_id);

// Whether any mod draws over the game, and drawing it. Inside an interface
// frame over the running game.
[[nodiscard]] bool mod_overlays_present();
void draw_mod_overlays();

} // namespace mhp3rd::ui
