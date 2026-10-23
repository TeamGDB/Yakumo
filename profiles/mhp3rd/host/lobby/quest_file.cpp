#include "lobby/quest_file.hpp"

#include <cstddef>
#include <utility>

namespace mhp3rd::lobby {
namespace {

constexpr std::size_t kMostQuestOffsets = 500u;
constexpr std::size_t kRecordHeader = 0x48u;

std::string read_text(std::span<const std::uint8_t> data, std::size_t &offset, std::size_t end) {
    // The game pads each field to four bytes. Skip the zero padding, or the
    // next field would be read from inside it and come back empty.
    while (offset < end && data[offset] == 0u) ++offset;
    std::string text;
    while (offset < end && data[offset] != 0u) text.push_back(static_cast<char>(data[offset++]));
    if (offset < end) ++offset;
    return text;
}

std::uint32_t load32(std::span<const std::uint8_t> data, std::size_t at) {
    return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1u]) << 8u |
        static_cast<std::uint32_t>(data[at + 2u]) << 16u | static_cast<std::uint32_t>(data[at + 3u]) << 24u;
}

// The catalog record's +0x20 byte is the map and time-of-day variant. These
// are the game's map ids, named in English so the dashboard does not need a
// translation file.
std::string zone_name(std::uint8_t id) {
    switch (id) {
    case 0x01:
        return "Deserted Island (Day)";
    case 0x02:
        return "Sandy Plains (Day)";
    case 0x03:
        return "Flooded Forest (Day)";
    case 0x04:
        return "Tundra (Day)";
    case 0x05:
        return "Volcano (Day)";
    case 0x06:
        return "Great Desert";
    case 0x07:
        return "Lava Canyon";
    case 0x08:
        return "Land Arena";
    case 0x09:
        return "Small Arena";
    case 0x0A:
        return "Sacred Land";
    case 0x0B:
        return "Polar Zone";
    case 0x0C:
        return "Misty Peaks (Day)";
    case 0x0D:
        return "Mountain Summit";
    case 0x0E:
        return "Deserted Island (Night)";
    case 0x0F:
        return "Sandy Plains (Night)";
    case 0x10:
        return "Flooded Forest (Night)";
    case 0x11:
        return "Tundra (Night)";
    case 0x12:
        return "Volcano (Night)";
    case 0x13:
        return "Misty Peaks (Night)";
    default:
        return "Unknown";
    }
}

// The Hall's star levels gate the hunter rank: 1-2 stars at HR1, 3-4 at
// HR2, 5 at HR3, 6 at HR4, 7 at HR5, and 8 at HR6. A normal quest has no
// separate join condition beyond this; the dashboard shows the star gate.
std::uint8_t minimum_rank(std::uint8_t stars) {
    if (stars <= 2u) return 1u;
    if (stars <= 4u) return 2u;
    if (stars <= 8u) return static_cast<std::uint8_t>(stars - 2u);
    return 6u;
}

} // namespace

std::vector<QuestFileEntry> parse_quest_file(std::span<const std::uint8_t> data) {
    std::vector<QuestFileEntry> quests;
    std::vector<std::uint32_t> offsets;
    for (std::size_t at = 0u; at + 4u <= data.size() && offsets.size() < kMostQuestOffsets; at += 4u) {
        const std::uint32_t value = load32(data, at);
        if (value == 0u) break;
        offsets.push_back(value);
    }
    if (offsets.empty() || offsets.front() != 4u * (offsets.size() + 1u)) return quests;
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        const std::uint32_t offset = offsets[i];
        const std::uint32_t end = i + 1u < offsets.size() ? offsets[i + 1u] : static_cast<std::uint32_t>(data.size());
        if (offset + kRecordHeader > data.size() || end > data.size() || end <= offset) continue;
        QuestFileEntry q;
        q.fee = load32(data, offset + 4u);
        q.reward = load32(data, offset + 8u);
        q.time_limit = load32(data, offset + 0x10u);
        q.id = static_cast<std::uint16_t>(data[offset + 0x1Cu] | data[offset + 0x1Du] << 8u);
        q.stars = data[offset + 0x1Eu];
        q.min_hr = minimum_rank(q.stars);
        q.zone = zone_name(data[offset + 0x20u]);
        std::size_t text = offset + kRecordHeader;
        q.name = read_text(data, text, end);
        q.objective = read_text(data, text, end);
        (void)read_text(data, text, end); // failure conditions
        (void)read_text(data, text, end); // description
        q.monsters = read_text(data, text, end);
        q.client = read_text(data, text, end);
        if (!q.name.empty()) quests.push_back(std::move(q));
    }
    return quests;
}

} // namespace mhp3rd::lobby
