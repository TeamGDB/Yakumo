#pragma once

// Shared identity of the local hunter's actual equipment, independent of the
// model shown by a preview or cosmetic override. Storage kinds and offsets
// are the traced NPJB-40001 facts documented in EQUIPMENT_MODS.md.
#include <cstdint>
#include <optional>

namespace mhp3rd::game {
class Ram;
}

namespace mhp3rd::game::weapon {

// Values are equipment-record kinds, not live actor/animation weapon indices.
enum class Type : std::uint8_t {
    GreatSword = 5,
    SwordAndShield = 6,
    Hammer = 7,
    Lance = 8,
    HeavyBowgun = 9,
    LightBowgun = 11,
    LongSword = 12,
    SwitchAxe = 13,
    Gunlance = 14,
    Bow = 15,
    DualBlades = 16,
    HuntingHorn = 17,
};

enum class Posture : std::uint8_t { Unknown, Sheathed, Drawn };

// Validated local actor in a quest; unavailable during loading or outside quests.
[[nodiscard]] Posture read_posture(const Ram &ram);
[[nodiscard]] Posture current_posture();

struct Equipped {
    Type type;
    std::uint16_t id;
    bool operator==(const Equipped &) const = default;
};

// Main-selector continuations, read from the live weapon transition table and
// its animation/input-buffer window. Unknown never means all actions disabled.
struct LongSwordActions {
    bool strike{}, thrust{}, fade{}, left{}, right{};
    bool operator==(const LongSwordActions &) const = default;
};
[[nodiscard]] std::optional<LongSwordActions> read_actions(const Ram &ram);
[[nodiscard]] std::optional<LongSwordActions> current_actions();

// No character, empty equipment or an unrecognized kind gives no identity.
// This reads only the equipped record; it does not scan inventory or models.
[[nodiscard]] std::optional<Equipped> read(const Ram &ram);

// Called once at each original game flip, before input/menu processing.
// Returns true when the type/id or availability changes, including removal.
// Read-only to guest RAM; no worker thread or save/settings writes.
bool refresh(const Ram &ram);

// Snapshot for host consumers on the game thread, between guest frames.
// Empty before the first valid refresh. Consumers must handle this explicitly.
[[nodiscard]] std::optional<Equipped> current();

} // namespace mhp3rd::game::weapon
