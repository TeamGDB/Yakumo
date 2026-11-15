#pragma once
namespace mhp3rd::ui {
// Read-only quest information, using the same game picture as other overlays.
void draw_combat_overlay();
void open_hp_layout_editor();
[[nodiscard]] bool hp_layout_editor_open();
void hp_layout_editor_frame(bool back);
}
