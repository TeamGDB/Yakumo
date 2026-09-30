#pragma once

// The System page's Text section, next to the language choice: importing a
// `.lang` file the player downloaded (a file chosen with the file browser, or
// dropped on the window) and opening the folder the translations are kept in.
// Importing copies the file into the per-user data directory's translations
// folder; a restart loads it, and it then appears in the language choice.
//
// The list the choice is built from is cached here, because scanning a folder
// and reading each file's header every frame would read a large `.lang` over and
// over. An import is the only thing that adds a file while the menu is open, so
// it rebuilds the list.

#include <string>
#include <vector>

namespace mhp3rd::ui {

// The languages the Text section offers: "Original" first, then every
// translation found, in a stable order.
struct TextLanguages {
    std::vector<std::string> codes;
    std::vector<std::string> names;
};
[[nodiscard]] const TextLanguages &text_languages();
void refresh_text_languages();

// The Text section's rows under the language choice.
void translation_rows();

// Whether the import screen is open.
[[nodiscard]] bool translation_screen_open();

// Draws the open import screen in place of the page. `back`: the back button was
// pressed this frame. True while the screen is open.
bool translation_screen(bool back);

} // namespace mhp3rd::ui
