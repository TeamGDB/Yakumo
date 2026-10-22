#pragma once

// What the Guild Hall shows about the hunter and the quests posted there,
// read from the game's memory. The hall keeps a small board with the four
// posted quests; each entry names a quest from the game's own DATA.BIN lists.
// The guild card title and comment sit beside the hunter's name in the board's
// player record.
//
// Pure functions over a game::Ram, so the unit tests run them on a buffer.

#include <cstdint>
#include <string>
#include <vector>

namespace mhp3rd::game {
class Ram;
}

namespace mhp3rd::lobby {

// One quest posted on the Guild Hall board.
struct PostedQuest {
    std::uint8_t slot{}; // 1 to 4, the board's order
    std::uint16_t id{};  // the quest id in DATA.BIN's lists
    std::uint8_t stars{};
    std::uint32_t fee{};
    std::uint32_t reward{};
    std::uint32_t time_limit{}; // frames at 30 a second
    std::string name;
    std::string objective;
    std::string monsters;
    std::string client;

    [[nodiscard]] bool operator==(const PostedQuest &other) const {
        return slot == other.slot && id == other.id && stars == other.stars && fee == other.fee &&
            reward == other.reward && time_limit == other.time_limit && name == other.name &&
            objective == other.objective && monsters == other.monsters && client == other.client;
    }
};

// The ids on the hall board, at most four, in the board's order. Pure: the
// tests use it on a buffer.
[[nodiscard]] std::vector<std::uint16_t> posted_quest_ids(const game::Ram &ram);

// The quests posted on the hall board, at most four, in the board's order.
// The ids are the game's; the details come from the DATA.BIN quest lists.
[[nodiscard]] std::vector<PostedQuest> posted_quests(const game::Ram &ram);

// The hunter rank kept beside the character's name.
[[nodiscard]] std::uint32_t hunter_rank(const game::Ram &ram);

// The guild card title and comment of the hunter called `name`, when the
// board's player record for it is in memory. Empty when it is not.
void guild_card(const game::Ram &ram, const std::string &name, std::string &title, std::string &comment);

// The board's memory: the posted entries live in this window. Exposed for the
// tests and for a future trace when a game update moves it.
inline constexpr std::uint32_t kQuestBoardBase = 0x09F56000u;
inline constexpr std::uint32_t kQuestBoardBytes = 0x00001000u;

} // namespace mhp3rd::lobby
