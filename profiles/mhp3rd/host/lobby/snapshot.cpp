#include "lobby/snapshot.hpp"

#include "game/equipment_models.hpp"
#include "game/game_data.hpp"
#include "lobby/diag.hpp"

#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>

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

std::string iso_time(std::time_t when) {
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &when);
#else
    gmtime_r(&when, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
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
    snapshot.quests = posted_quests(ram);

    // Remember when each posted quest first appeared, so the dashboard can
    // show how long it has been running. A new board resets the clock.
    static std::string last_hall;
    static std::vector<std::uint16_t> last_ids;
    static std::map<std::uint16_t, std::string> first_seen;
    std::vector<std::uint16_t> ids;
    for (const PostedQuest &quest : snapshot.quests) ids.push_back(quest.id);
    if (last_hall != snapshot.hall || ids != last_ids) {
        first_seen.clear();
        last_ids = ids;
        last_hall = snapshot.hall;
    }
    if (snapshot.quests.empty()) first_seen.clear();
    if (!snapshot.quests.empty()) {
        const std::string now = iso_time(std::time(nullptr));
        for (PostedQuest &quest : snapshot.quests) {
            const auto entry = first_seen.emplace(quest.id, now);
            quest.started_at = entry.first->second;
            quest.poster = snapshot.name;
        }
    }
    maybe_dump(ram, snapshot);
}

} // namespace mhp3rd::lobby
