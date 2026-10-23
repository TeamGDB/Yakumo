#pragma once

// Quest payloads built from a description, with no bytes of the disc's
// quests: the layout docs/QUESTS.md describes, with each table either filled
// from the description or in the form the game reads as empty. Built this
// way, a hunt in the arena (map 8) loaded, spawned its monster with the
// health and size asked for, and was cleared when the monster fell.
//
// The game's quest files hold more (small monsters, gathering, supplies
// delivered later): a built quest has none of those yet. Its rewards and the
// supplies in its box are what it is given.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mhp3rd::game {

struct QuestMonster {
    std::uint16_t kind{};
    std::uint16_t area{};       // of the quest's map
    float x{}, y{}, z{};        // where it starts in the area
    std::uint8_t health{};      // the game's health level (higher is more; 0x1E gave a Tigrex-based kind 5400)
    std::uint8_t attack{};      // the game's attack level (0x13 a low rank hunt, 0x30 hit three times as hard)
    std::uint16_t size{100};    // percent of the kind's size
};

struct QuestTexts {
    std::string name, objective, failure, description, monsters, client;
};

// One item of a reward group: drawn with `chance` percent, `count` of item `item`.
struct QuestRewardItem {
    std::uint16_t item{};
    std::uint8_t count{1};
    std::uint8_t chance{100};
};
// A reward group: items whose chances add up to 100, drawn from. The game's
// quests have up to three: the main reward, drawn several times, and two more
// (a guild's commendation, say).
using QuestRewardGroup = std::vector<QuestRewardItem>;
inline constexpr std::size_t kMostRewardGroups = 3u;
// What the supply box holds at the start: `count` of item `item`.
struct QuestSupply {
    std::uint16_t item{};
    std::uint8_t count{1};
};
inline constexpr std::size_t kMostSupplies = 32u;

struct QuestSpec {
    std::uint16_t id{};          // 62000 to 65535, as mods' quests are
    std::uint8_t map{};
    std::uint16_t stars{};
    std::uint32_t minutes{50};
    std::uint32_t fee{};
    std::uint32_t reward{};
    std::uint16_t target{};      // the objective: hunt `count` of this kind
    std::uint16_t count{1};
    std::vector<QuestMonster> monsters; // large monsters, up to 5; the first ones are the main monsters
    QuestTexts texts;            // in every language the game has, for now
    std::vector<QuestRewardGroup> rewards; // drawn from when the quest is cleared: the main reward first
    std::vector<QuestSupply> supplies;     // in the supply box
};

// The payload, or nothing (with the reason in `why`) for a description the
// game could not take: no monsters or more than five, an id outside mods'
// quests, rewards or supplies the game does not have room for or naming no
// item, a payload over the size the game reads.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> build_quest(const QuestSpec &spec, std::string *why = nullptr);

} // namespace mhp3rd::game
