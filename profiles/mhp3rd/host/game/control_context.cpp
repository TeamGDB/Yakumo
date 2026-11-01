#include "game/control_context.hpp"
#include "game/guest_ram.hpp"
#include <string_view>

namespace mhp3rd::game::control_context {
namespace {
constexpr std::uint32_t kTask = 0x0A05E600u;
constexpr std::uint32_t kHubState = 0x0A25DD28u;
constexpr std::uint32_t kMenu = 0x09FF53B0u;
Context snapshot;
bool named(const Ram &ram, std::string_view name) {
    if (!ram.contains(kTask, 64u) || ram.load32(kTask) != 0x336F574Du || ram.load32(kTask + 8u) != kTask) return false;
    for (std::size_t i = 0; i < name.size(); ++i)
        if (ram.load8(kTask + 32u + static_cast<std::uint32_t>(i)) != name[i]) return false;
    return ram.load8(kTask + 32u + static_cast<std::uint32_t>(name.size())) == 0u;
}
bool object(const Ram &ram, std::uint32_t slot, std::size_t size, std::uint32_t &out) {
    if (!ram.contains(slot, 4u)) return false;
    out = ram.load32(slot);
    return out >= 0x08800000u && out < 0x0C000000u && !(out & 3u) &&
        size <= 0x0C000000u - out && ram.contains(out, size);
}
}
Context read(const Ram &ram) {
    std::uint32_t p{};
    if (named(ram, "lobby_task.ovl")) {
        // The lobby dispatcher: zero while walking, nonzero for conversations,
        // menus and departure. See the traced quest-start facts in DEBUG_MENU.
        if (!object(ram, kHubState, 4u, p)) return {Scene::Hub, true};
        return {Scene::Hub, ram.load32(p) != 0u};
    }
    if (named(ram, "game_task.ovl")) {
        // NPJB-40001 menu manager: +4 is Start's active state; +0x18
        // suppresses it while an interaction UI (such as supplies) owns input.
        // +0xB95 is an additional activation byte used by its active-state query. Observed
        // independently with Start, supplies and the return to walking.
        if (!object(ram, kMenu, 0xB96u, p)) return {Scene::Quest, true};
        return {Scene::Quest, ram.load8(p + 4u) != 0u || ram.load8(p + 0x18u) != 0u || ram.load8(p + 0xB95u) != 0u};
    }
    return {};
}
bool refresh(const Ram &ram) {
    const auto next = read(ram);
    const bool changed = next != snapshot;
    snapshot = next;
    return changed;
}
Context current() { return snapshot; }
} // namespace mhp3rd::game::control_context
