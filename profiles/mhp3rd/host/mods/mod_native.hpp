#pragma once

#include "mods/mod_library.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace psprecomp {
class Runtime;
}

struct ymod_host;
struct ymod_mod;
struct ymod_widgets;

// Native mods: shared libraries mods ship for the platform Yakumo runs on,
// loaded when the game starts with them on. They see the game through the
// services in sdk/include/yakumo/mod.h, implemented here: guest memory, hooks
// on the game's functions and instructions, calls into guest code, events,
// files, settings and services between mods.
//
// With no native mod on, nothing here runs and nothing is hooked.
namespace mhp3rd::mods::native {

// Loads the libraries of `sources`, in order, and calls each one's entry
// point. Once, after the code mods are written and before the game runs.
void start(psprecomp::Runtime &runtime, const std::vector<Resolution::Source> &sources);

// For mods that are not libraries, script mods: the host interface native
// mods get, a mod object of their own, and the runtime they act on.
[[nodiscard]] const struct ::ymod_host *host();
[[nodiscard]] struct ::ymod_mod *add_mod(const std::string &id, const std::string &name, const std::string &folder);
void attach(psprecomp::Runtime &runtime);

// Events from the rest of the host.
void frame(psprecomp::Runtime &runtime);
void file_loaded(FileId file);
void overlay_loaded(psprecomp::Runtime &runtime, std::uint32_t base);

// What mods draw in the interface ("yakumo.ui"): the panels of the mod `mod`
// for its page in the Mods menu, or every mod's overlays (`overlays`) for over
// the running game. The interface calls draw with its widgets.
struct Panel {
    std::string mod; // the mod's name
    std::string title;
    void (*draw)(const struct ::ymod_widgets *ui, void *user){};
    void *user{};
};
[[nodiscard]] std::vector<Panel> panels(const std::string &mod, bool overlays);

// What a native mod logged and what failed, for the Mods page; newest last.
struct Report {
    std::string mod;
    std::string line;
    bool problem{};
};
[[nodiscard]] const std::vector<Report> &reports();
// Takes down everything a mod registered: its hooks, event subscriptions,
// panels, overlays and services. For a mod that refused to start or a script
// that failed: nothing of it is called again.
void withdraw(struct ::ymod_mod *mod);

// How many native mods run now.
[[nodiscard]] std::size_t running();
// Whether a mod is still called: not once it refused to start, failed or was
// withdrawn. What it added to the game stays (docs/MODDING.md, "Native
// mods"), but nothing of its code runs again. Null, for Yakumo's own users
// of the API, always runs.
[[nodiscard]] bool runs(const struct ::ymod_mod *mod);

// Forgets everything, so tests can start again. Libraries stay loaded.
void reset_for_tests();

} // namespace mhp3rd::mods::native
