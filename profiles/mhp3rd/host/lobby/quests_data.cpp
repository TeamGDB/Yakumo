#include "lobby/quests.hpp"

#include "game/guest_ram.hpp"
#include "lobby/quest_file.hpp"
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

const std::vector<QuestFileEntry> &game_quests() {
    static const std::vector<QuestFileEntry> quests = [] {
        std::vector<QuestFileEntry> all;
        for (std::uint32_t level = 0u; level < kQuestLists; ++level) {
            const std::optional<std::vector<std::uint8_t>> file = read_entry(kFirstQuestList + level);
            if (!file) continue;
            for (QuestFileEntry &quest : parse_quest_file(*file)) all.push_back(std::move(quest));
        }
        return all;
    }();
    return quests;
}

const QuestFileEntry *find_quest(std::uint16_t id) {
    for (const QuestFileEntry &quest : game_quests())
        if (quest.id == id) return &quest;
    return nullptr;
}

} // namespace

std::vector<PostedQuest> posted_quests(const game::Ram &ram) {
    std::vector<PostedQuest> result;
    const std::uint16_t id = active_quest_id(ram);
    if (id == 0u) return result;
    PostedQuest quest;
    quest.slot = 1u;
    quest.id = id;
    if (const QuestFileEntry *info = find_quest(quest.id)) {
        quest.stars = info->stars;
        quest.min_hr = info->min_hr;
        quest.fee = info->fee;
        quest.reward = info->reward;
        quest.time_limit = info->time_limit;
        quest.zone = info->zone;
        quest.name = info->name;
        quest.objective = info->objective;
        quest.monsters = info->monsters;
        quest.client = info->client;
    }
    // The selected record carries the star level and the map beside its id;
    // prefer it when it is in memory, so a modded or translated catalog cannot
    // change what the board is showing. DATA.BIN remains the fallback.
    if (ram.contains(kActiveQuestRecord + kActiveQuestStarsOffset, 1u)) {
        const std::uint8_t stars = ram.load8(kActiveQuestRecord + kActiveQuestStarsOffset);
        if (stars >= 1u && stars <= 8u) {
            quest.stars = stars;
            quest.min_hr = minimum_rank(stars);
        }
    }
    if (ram.contains(kActiveQuestRecord + kActiveQuestZoneOffset, 1u)) {
        const std::uint8_t zone = ram.load8(kActiveQuestRecord + kActiveQuestZoneOffset);
        if (zone != 0u) quest.zone = zone_name(zone);
    }
    result.push_back(std::move(quest));
    return result;
}

} // namespace mhp3rd::lobby
