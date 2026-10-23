#include "lobby/quests.hpp"

#include "mods/mhp3rd_data_bin.hpp"
#include "mods/mhp3rd_mods.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mhp3rd::lobby {
namespace {

// The game's quest lists: entries 4059 to 4066, one per star level (see
// host/debug/quest_start.hpp).
constexpr std::uint32_t kFirstQuestList = 4059u;
constexpr std::uint32_t kQuestLists = 8u;
constexpr std::size_t kMostQuestOffsets = 500u;

struct QuestInfo {
    std::uint16_t id{};
    std::uint8_t stars{};
    std::uint32_t fee{};
    std::uint32_t reward{};
    std::uint32_t time_limit{};
    std::string name;
    std::string objective;
    std::string monsters;
    std::string client;
};

// A DATA.BIN entry as the game sees it, with a mod's files when mods are on.
std::optional<std::vector<std::uint8_t>> read_entry(std::uint32_t entry) {
    namespace db = mods::p3rd;
    if (mods::data_bin_size() == 0u) return std::nullopt;
    std::vector<std::uint8_t> head(db::kBlock);
    head.resize(mods::read_data_bin(0u, head));
    if (head.size() < 4u) return std::nullopt;
    std::vector<std::uint8_t> first(head.begin(), head.begin() + 4);
    db::decrypt(first, 0u, 0u);
    const std::uint32_t blocks =
        first[0] | first[1] << 8u | first[2] << 16u | static_cast<std::uint32_t>(first[3]) << 24u;
    if (blocks == 0u || blocks >= 512u) return std::nullopt;
    std::vector<std::uint8_t> encrypted(static_cast<std::size_t>(blocks) * db::kBlock);
    encrypted.resize(mods::read_data_bin(0u, encrypted));
    const std::optional<db::Directory> directory = db::Directory::parse(encrypted, mods::data_bin_size());
    if (!directory || entry >= directory->entries()) return std::nullopt;
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(directory->size(entry)));
    const std::uint32_t block = directory->blocks[entry];
    bytes.resize(mods::read_data_bin(static_cast<std::uint64_t>(block) * db::kBlock, bytes));
    if (!db::verbatim_magic(bytes)) db::decrypt(bytes, block, 0u);
    return bytes;
}

std::string read_text(std::span<const std::uint8_t> data, std::size_t &offset) {
    std::string text;
    while (offset < data.size() && data[offset] != 0u) text.push_back(static_cast<char>(data[offset++]));
    if (offset < data.size()) ++offset;
    return text;
}

std::vector<QuestInfo> parse_quest_list(std::span<const std::uint8_t> data) {
    std::vector<QuestInfo> quests;
    std::vector<std::uint32_t> offsets;
    for (std::size_t at = 0u; at + 4u <= data.size() && offsets.size() < kMostQuestOffsets; at += 4u) {
        const std::uint32_t value = static_cast<std::uint32_t>(data[at]) |
            static_cast<std::uint32_t>(data[at + 1u]) << 8u | static_cast<std::uint32_t>(data[at + 2u]) << 16u |
            static_cast<std::uint32_t>(data[at + 3u]) << 24u;
        if (value == 0u) break;
        offsets.push_back(value);
    }
    if (offsets.empty() || offsets.front() != 4u * (offsets.size() + 1u)) return quests;
    for (const std::uint32_t offset : offsets) {
        if (offset + 0x48u > data.size()) continue;
        QuestInfo q;
        q.fee = static_cast<std::uint32_t>(data[offset + 4u]) | static_cast<std::uint32_t>(data[offset + 5u]) << 8u |
            static_cast<std::uint32_t>(data[offset + 6u]) << 16u | static_cast<std::uint32_t>(data[offset + 7u]) << 24u;
        q.reward = static_cast<std::uint32_t>(data[offset + 8u]) | static_cast<std::uint32_t>(data[offset + 9u]) << 8u |
            static_cast<std::uint32_t>(data[offset + 10u]) << 16u |
            static_cast<std::uint32_t>(data[offset + 11u]) << 24u;
        q.time_limit = static_cast<std::uint32_t>(data[offset + 0x10u]) |
            static_cast<std::uint32_t>(data[offset + 0x11u]) << 8u |
            static_cast<std::uint32_t>(data[offset + 0x12u]) << 16u |
            static_cast<std::uint32_t>(data[offset + 0x13u]) << 24u;
        q.id = static_cast<std::uint16_t>(data[offset + 0x1Cu] | data[offset + 0x1Du] << 8u);
        q.stars = data[offset + 0x1Eu];
        std::size_t text = offset + 0x48u;
        q.name = read_text(data, text);
        q.objective = read_text(data, text);
        (void)read_text(data, text); // failure conditions
        (void)read_text(data, text); // description
        q.monsters = read_text(data, text);
        q.client = read_text(data, text);
        if (!q.name.empty()) quests.push_back(std::move(q));
    }
    return quests;
}

const std::vector<QuestInfo> &game_quests() {
    static const std::vector<QuestInfo> quests = [] {
        std::vector<QuestInfo> all;
        for (std::uint32_t level = 0u; level < kQuestLists; ++level) {
            const std::optional<std::vector<std::uint8_t>> file = read_entry(kFirstQuestList + level);
            if (!file) continue;
            for (QuestInfo &quest : parse_quest_list(*file)) all.push_back(std::move(quest));
        }
        return all;
    }();
    return quests;
}

const QuestInfo *find_quest(std::uint16_t id) {
    for (const QuestInfo &quest : game_quests())
        if (quest.id == id) return &quest;
    return nullptr;
}

} // namespace

std::vector<PostedQuest> posted_quests(const game::Ram &ram) {
    std::vector<PostedQuest> result;
    const std::uint16_t id = latest_quest_id(ram);
    if (id == 0u) return result;
    PostedQuest quest;
    quest.slot = 1u;
    quest.id = id;
    if (const QuestInfo *info = find_quest(quest.id)) {
        quest.stars = info->stars;
        quest.fee = info->fee;
        quest.reward = info->reward;
        quest.time_limit = info->time_limit;
        quest.name = info->name;
        quest.objective = info->objective;
        quest.monsters = info->monsters;
        quest.client = info->client;
    }
    result.push_back(std::move(quest));
    return result;
}

} // namespace mhp3rd::lobby
