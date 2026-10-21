#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace psprecomp {
class GuestMemory;
}

// What the game does without telling anyone, for the mod events
// (sdk/include/yakumo/mod.h): a code overlay leaving its slot, because the
// game copies the next one or data over it, and a quest beginning or ending,
// read from the quest overlay in the task slot and the accepted quest's id
// (docs/DEBUG_MENU.md).
namespace mhp3rd::mods::p3rd {

class GameWatch {
public:
    // Told with a YMOD_EVENT_* and its value.
    using Fire = void (*)(int event, std::uint32_t value);

    // A code overlay was copied over `base` (where the game flushed): the
    // overlay's own load address, or 0 when no overlay is there or this load
    // was already told: the game flushes twice for one overlay, in one frame.
    // Overlays it replaced must have been looked for first (look()).
    std::uint32_t overlay_loaded(const psprecomp::GuestMemory &memory, std::uint32_t base);

    // A frame has passed: an overlay copied again from now on is a new load.
    void next_frame() { told_.clear(); }

    // Overlays gone since the last look, then a quest that began or ended.
    void look(const psprecomp::GuestMemory &memory, Fire fire);

    // The quest being played, by id.
    [[nodiscard]] std::optional<std::uint32_t> quest() const { return quest_; }

private:
    std::map<std::uint32_t, std::string> overlays_; // loaded now, by load address
    std::map<std::uint32_t, std::string> told_;     // loads told this frame
    std::optional<std::uint32_t> quest_;
};

} // namespace mhp3rd::mods::p3rd
