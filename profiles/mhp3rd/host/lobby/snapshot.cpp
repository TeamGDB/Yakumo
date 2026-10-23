#include "lobby/snapshot.hpp"

#include "game/equipment_models.hpp"
#include "game/game_data.hpp"
#include "lobby/diag.hpp"

namespace mhp3rd::lobby {
namespace {

// The game's armor kinds in its own order, chest first; the dashboard's slots
// are the same five parts with friendlier names.
const char *slot_label(std::uint8_t kind) {
    switch (kind) {
    case 0u:
        return "chest";
    case 1u:
        return "arms";
    case 2u:
        return "waist";
    case 3u:
        return "legs";
    case 4u:
        return "head";
    default:
        return "armor";
    }
}

} // namespace

void read_hunter(const game::Ram &ram, Snapshot &snapshot) {
    if (const std::string name = game::hunter_name(ram); !name.empty()) snapshot.name = name;
    if (const std::optional<game::Look> look = game::hunter_look(ram)) snapshot.sex = game::sex_name(look->sex);

    snapshot.equipment.clear();
    for (std::uint8_t kind = 0u; kind <= 4u; ++kind) {
        const std::optional<game::Piece> piece = game::worn_armor(ram, kind);
        if (!piece || piece->id == 0u) continue;
        snapshot.equipment.push_back(Piece{slot_label(kind), piece->id, game::equipment_name(ram, kind, piece->id)});
    }

    snapshot.weapon.reset();
    if (const std::optional<game::Piece> weapon = game::carried_weapon(ram)) {
        const game::EquipmentKind *kind = game::equipment_kind(weapon->kind);
        snapshot.weapon = Weapon{weapon->kind, weapon->id, game::equipment_name(ram, weapon->kind, weapon->id),
            kind != nullptr ? kind->label : std::string()};
    }

    snapshot.hr = static_cast<int>(hunter_rank(ram));
    if (!snapshot.name.empty()) guild_card(ram, snapshot.name, snapshot.title, snapshot.guild_card);
    guild_stats(ram, snapshot.stats);
    // The hall board is not the guild-card history window at 0x9F56000. Until
    // the live board is traced, do not report that history as the active
    // quest; the diagnostic dump captures the hall for the next fix.
    snapshot.quests.clear();
    maybe_dump(ram, snapshot);
}

} // namespace mhp3rd::lobby
