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

// One labelled count from the guild card.
struct StatCount {
    std::string label;
    std::uint32_t count{};

    [[nodiscard]] bool operator==(const StatCount &other) const { return label == other.label && count == other.count; }
};

// The guild card's summary: quest counts, felyne level, play time, weapon use,
// recent quests and the award count. All fields are empty or zero until the
// card is in memory.
struct GuildStats {
    std::vector<StatCount> quests;
    std::uint32_t fs_level{};
    std::uint32_t play_seconds{};
    std::vector<StatCount> weapons;
    std::vector<std::string> recent_quests;
    std::uint32_t awards{};

    [[nodiscard]] bool empty() const {
        return quests.empty() && fs_level == 0u && play_seconds == 0u && weapons.empty() && recent_quests.empty() &&
            awards == 0u;
    }
    [[nodiscard]] bool operator==(const GuildStats &other) const {
        return quests == other.quests && fs_level == other.fs_level && play_seconds == other.play_seconds &&
            weapons == other.weapons && recent_quests == other.recent_quests && awards == other.awards;
    }
};

// One quest posted on the Guild Hall board.
struct PostedQuest {
    std::uint8_t slot{}; // 1 to 4, the board's order
    std::uint16_t id{};  // the quest id in DATA.BIN's lists
    std::uint8_t stars{};
    std::uint8_t min_hr{}; // lowest hunter rank the star level admits
    std::uint32_t fee{};
    std::uint32_t reward{};
    std::uint32_t time_limit{}; // frames at 30 a second
    std::string zone;           // the map and time of day
    std::string name;
    std::string objective;
    std::string monsters;
    std::string client;
    std::string poster;     // the hunter who posted it in the hall
    std::string accepted;   // who accepted the quest, when the board reports it
    std::string password;   // the hall's password, empty for an open hall
    std::string note;       // the hall's recruitment note
    std::string started_at; // quest start time, ISO 8601, when the board reports it

    [[nodiscard]] bool operator==(const PostedQuest &other) const {
        return slot == other.slot && id == other.id && stars == other.stars && min_hr == other.min_hr &&
            fee == other.fee && reward == other.reward && time_limit == other.time_limit && zone == other.zone &&
            name == other.name && objective == other.objective && monsters == other.monsters &&
            client == other.client && poster == other.poster && accepted == other.accepted &&
            password == other.password && note == other.note && started_at == other.started_at;
    }
};

// The ids on the hall board, at most four, in the board's order. Pure: the
// tests use it on a buffer.
[[nodiscard]] std::vector<std::uint16_t> posted_quest_ids(const game::Ram &ram);

// The quest most recently posted in the hall. The board keeps older entries
// in the same window; the last new id is the one on screen.
[[nodiscard]] std::uint16_t latest_quest_id(const game::Ram &ram);

// The quest record the hall's board currently shows. The game keeps the
// selected quest's info at 0x0A2E9600; the id is at +0x24.
inline constexpr std::uint32_t kActiveQuestRecord = 0x0A2E9600u;
inline constexpr std::uint32_t kActiveQuestIdOffset = 0x24u;
inline constexpr std::uint32_t kActiveQuestStarsOffset = 0x26u;
inline constexpr std::uint32_t kActiveQuestZoneOffset = 0x28u;
[[nodiscard]] std::uint16_t active_quest_id(const game::Ram &ram);

// The quests posted on the hall board, at most four, in the board's order.
// The ids are the game's; the details come from the DATA.BIN quest lists.
[[nodiscard]] std::vector<PostedQuest> posted_quests(const game::Ram &ram);

// The hunter rank, a byte in the character block.
[[nodiscard]] std::uint32_t hunter_rank(const game::Ram &ram);

// The guild card title and comment of the hunter called `name`, from the
// hall's own-card list. The local hunter's card is its first record. Empty
// when the list is not in memory.
void guild_card(const game::Ram &ram, const std::string &name, std::string &title, std::string &comment);

// The guild card's summary, read from the running game.
void guild_stats(const game::Ram &ram, GuildStats &stats);

// The board's memory: the posted entries live in this window. Exposed for the
// tests and for a future trace when a game update moves it.
inline constexpr std::uint32_t kQuestBoardBase = 0x09F56000u;
inline constexpr std::uint32_t kQuestBoardBytes = 0x00001000u;

// The hall's guild-card list. The local hunter's card is the first record;
// the live records use the title at +0x54 and the comment at +0x6E.
inline constexpr std::uint32_t kGuildCardListBase = 0x09FA8000u;
inline constexpr std::uint32_t kGuildCardListBytes = 0x00005000u;

// The guild-card summary fields, traced from a live hall dump.
inline constexpr std::uint32_t kQuestCountsBase = 0x09FA82D0u; // 5 u16
inline constexpr std::uint32_t kPlayTime = 0x09F54544u;        // u32 seconds
inline constexpr std::uint32_t kWeaponUsageBase = 0x09FA86C0u; // kind 5..17, u16 each
inline constexpr std::uint32_t kFelyneLevelsBase = 0x09FACF3Fu;
inline constexpr std::uint32_t kFelyneLevelStride = 0xA0u;
inline constexpr std::uint32_t kRecentQuestsBase = 0x09FA86F4u;
inline constexpr std::uint32_t kRecentQuestsBytes = 0x00000690u;
inline constexpr std::uint32_t kAwardsBase = 0x09FA8344u;
inline constexpr std::uint32_t kAwardsBytes = 0x00000300u;

} // namespace mhp3rd::lobby
