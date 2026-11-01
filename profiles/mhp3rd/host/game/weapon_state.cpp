#include "game/weapon_state.hpp"

#include "game/equipment_models.hpp"
#include "game/control_context.hpp"
#include "game/guest_ram.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace mhp3rd::game::weapon {
namespace {
Posture posture = Posture::Unknown;
std::optional<LongSwordActions> actions;
bool mapped(const Ram &ram, std::uint32_t address, std::size_t size) {
    return address >= 0x08800000u && address < 0x0C000000u && !(address & 3u) &&
        size <= 0x0C000000u - address && ram.contains(address, size);
}
std::optional<Equipped> &snapshot() {
    static std::optional<Equipped> value;
    return value;
}
} // namespace

std::optional<Equipped> read(const Ram &ram) {
    const auto piece = carried_weapon(ram);
    if (!piece || piece->id == 0u) return std::nullopt;
    // carried_weapon rejects armor, the unused kind 10 and unknown kinds.
    return Equipped{static_cast<Type>(piece->kind), piece->id};
}

namespace {
std::uint32_t local_actor(const Ram &ram) {
    if (!read(ram) || control_context::read(ram).scene != control_context::Scene::Quest)
        return 0u;
    // NPJB-40001: the camera calls the six-slot actor lookup at 0x088B6B30.
    // Validate its table access and the camera's test of actor +0x27C bit 3.
    if (!ram.contains(0x088B6B68u, 4u) || ram.load32(0x088B6B68u) != 0x8C420008u ||
        !ram.contains(0x088E49C8u, 4u) || ram.load32(0x088E49C8u) != 0x30420008u ||
        !ram.contains(0x09FBE794u, 4u)) return 0u;
    const auto table = ram.load32(0x09FBE794u);
    if (!mapped(ram, table, 8u + 6u * 4u)) return 0u;
    std::uint32_t local{};
    for (unsigned slot = 0; slot < 6u; ++slot) {
        const auto actor = ram.load32(table + 8u + slot * 4u);
        if (!mapped(ram, actor, 0x280u) || ram.load16(actor + 0x60u) != 0u) continue;
        const auto vtable = ram.load32(actor);
        if (!mapped(ram, vtable, 0xACu) || ram.load32(vtable + 0xA8u) != 0x088A58DCu) continue;
        if (local && local != actor) return 0u;
        local = actor;
    }
    if (!local) return 0u;
    return local;
}
}
Posture read_posture(const Ram &ram) {
    const auto actor = local_actor(ram);
    if (!actor) return Posture::Unknown;
    return (ram.load32(actor + 0x27Cu) & 8u) ? Posture::Drawn : Posture::Sheathed;
}

std::optional<LongSwordActions> read_actions(const Ram &ram) {
    const auto equipped = read(ram);
    if (!equipped || equipped->type != Type::LongSword || control_context::read(ram).interface_open)
        return std::nullopt;
    const auto actor = local_actor(ram);
    if (!actor || !ram.contains(actor, 0x14B4u) || ram.load8(actor + 0x62u) != 7u ||
        !(ram.load32(actor + 0x27Cu) & 8u)) return std::nullopt;
    // NPJB-40001's action-table dispatcher, request predicate and animation accessor.
    if (!ram.contains(0x0A10A3CCu, 4u) || ram.load32(0x0A10A3CCu) != 0x8C8214A8u ||
        !ram.contains(0x0A10790Cu, 4u) || ram.load32(0x0A10790Cu) != 0x8C620304u ||
        !ram.contains(0x088A687Cu, 4u) || ram.load32(0x088A687Cu) != 0x24820F80u ||
        ram.load32(ram.load32(actor) + 0x44u) != 0x088A687Cu) return std::nullopt;
    LongSwordActions result;
    const auto state = ram.load8(actor + 0x220u);
    if (state == 8u) return result; // Observed draw/sheathe/evade transition class.
    if (state != 0u && state != 9u) return std::nullopt;
    const auto table = ram.load32(actor + 0x14A8u);
    // The original dispatcher also refuses a missing/suspended transition table.
    if (!table || static_cast<std::int8_t>(ram.load8(actor + 0x1475u)) > 0) return result;
    if (table < 0x08800000u || table >= 0x0C000000u || (table & 1u)) return std::nullopt;
    const auto real = [&](std::uint32_t offset) { return std::bit_cast<float>(ram.load32(actor + offset)); };
    const float frame = real(0x0F84u), step = real(0x0F8Cu), duration = real(0x0F94u);
    if (!std::isfinite(frame) || !std::isfinite(step) || !std::isfinite(duration) || frame < 0 || step < 0 ||
        duration <= 0 || frame > duration) return std::nullopt;
    const auto flags = ram.load16(actor + 0x0FD0u);
    const bool stopped = (flags & 2u) || static_cast<std::int32_t>(ram.load32(actor + 0x0FC8u)) > 0 ||
        (ram.load32(actor + 0x0FA8u) == 1u && !(ram.load32(actor + 0x0FC0u) & 1u));
    const float next = std::min(frame + (stopped ? 0.f : step), duration);
    for (unsigned row = 0; row < 48u; ++row) {
        const auto entry = table + row * 22u;
        if (entry >= 0x0C000000u || !ram.contains(entry, 22u)) return std::nullopt;
        const auto kind = ram.load16(entry);
        if (kind == 0xFFu) return result;
        // Only ordinary weapon-input routes: utility/automatic/guard predicates
        // are not substituted for main-selector attacks.
        if (kind != 4u || (ram.load16(entry + 6u) & 0x3FFFu) != 25u) continue;
        const unsigned request = ram.load16(entry + 8u);
        constexpr std::uint32_t strike = (1u << 0) | (1u << 3) | (1u << 8);
        constexpr std::uint32_t thrust = (1u << 2) | (1u << 5) | (1u << 14);
        constexpr std::uint32_t fade = (1u << 1) | (1u << 4);
        if (request >= 32u || !(((strike | thrust | fade | (3u << 17)) >> request) & 1u)) continue;
        if (ram.load16(entry + 0x0Au) || ram.load16(entry + 0x0Cu) || ram.load16(entry + 0x0Eu))
            return std::nullopt; // An additional unverified condition must not block a valid gesture.
        const auto threshold = [&](unsigned offset, unsigned override_offset) {
            const auto replacement = ram.load16(actor + override_offset);
            return replacement ? static_cast<int>(replacement) : static_cast<int>(static_cast<std::int16_t>(ram.load16(entry + offset)));
        };
        int first = threshold(16u, 0x14ACu), last = threshold(18u, 0x14B0u), buffer = threshold(20u, 0x14AEu);
        if (first >= 999 || last >= 999 || buffer < 0 || buffer >= 999) return std::nullopt;
        const float begin = (first < 0 ? duration + first : static_cast<float>(first)) - buffer;
        const float end = last == 0 ? duration : last < 0 ? duration + last : std::min(duration, static_cast<float>(last));
        if (end < 0 || begin > end) return std::nullopt;
        if (next < begin || next > end) continue;
        result.strike |= ((strike >> request) & 1u) != 0;
        result.thrust |= ((thrust >> request) & 1u) != 0;
        result.fade |= ((fade >> request) & 1u) != 0;
        result.left |= request == 17u;
        result.right |= request == 18u;
    }
    return std::nullopt; // No sentinel within the game's bounded row capacity.
}
std::optional<LongSwordActions> current_actions() { return actions; }

Posture current_posture() { return posture; }

bool refresh(const Ram &ram) {
    posture = read_posture(ram);
    actions = read_actions(ram);
    const auto next = read(ram);
    auto &previous = snapshot();
    const bool changed = previous != next;
    previous = next;
    return changed;
}

std::optional<Equipped> current() {
    return snapshot();
}

} // namespace mhp3rd::game::weapon
