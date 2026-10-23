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
