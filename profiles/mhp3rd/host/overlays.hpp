#pragma once

#include "mhp3rd_profile.hpp"

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace mhp3rd {

// Loads the overlay libraries and installs the dispatch-miss hook that
// recognises the overlay currently in a slot and registers its recompiled
// corpus. The libraries are read from MHP3RD_OVERLAY_DIR, or from overlays/
// next to the executable. With MHP3RD_DUMP_OVERLAYS set, an unknown overlay is
// written out instead so it can be recompiled.
void install_overlay_support(psprecomp::Runtime &runtime);

// Drops the corpus of any slot whose contents no longer match it. The guest
// flushes the instruction cache right after loading an overlay, which is when
// this is called; the next jump into the slot then installs the right corpus.
void revalidate_overlays(psprecomp::Runtime &runtime);

// Installs the recompiled code of the overlay the game has just copied to
// `base`, as the first dispatch into it would, so that a mod can change some
// of its code afterwards and only the units it changes leave native code. True
// when the overlay's corpus is installed, now or before.
bool install_overlay_at(psprecomp::Runtime &runtime, std::uint32_t base);

// The code overlay loaded now whose image covers `address`: its name without
// the extension ("game_task") and where it loads. Nothing when no overlay
// covers it.
struct LoadedOverlay {
    std::string name;
    std::uint32_t base{};
};
[[nodiscard]] std::optional<LoadedOverlay> overlay_at(const psprecomp::GuestMemory &memory, std::uint32_t address);

// The guest loaded code: a slot whose image matched no corpus is searched
// again at the next call into it.
void forget_unmatched_overlays();

} // namespace mhp3rd
