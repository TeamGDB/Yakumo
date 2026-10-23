#pragma once

// One quest from a DATA.BIN quest list, before the lobby adds the board's
// context and the hunter who posted it. The game's lists are archive entries
// 4059-4066; each is an array of record offsets followed by the records.
//
// Pure over the file bytes, so the parser runs in the unit tests on a buffer
// without the game's data.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace mhp3rd::lobby {

struct QuestFileEntry {
    std::uint16_t id{};
    std::uint8_t stars{};
    std::uint8_t min_hr{}; // lowest hunter rank the star level admits
    std::uint32_t fee{};
    std::uint32_t reward{};
    std::uint32_t time_limit{};
    std::string zone; // the quest's map and time of day
    std::string name;
    std::string objective;
    std::string monsters;
    std::string client;
};

[[nodiscard]] std::vector<QuestFileEntry> parse_quest_file(std::span<const std::uint8_t> data);

} // namespace mhp3rd::lobby
