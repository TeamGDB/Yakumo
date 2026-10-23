#include "lobby/quests.hpp"

#include "game/game_data.hpp"
#include "game/guest_ram.hpp"

#include <algorithm>

namespace mhp3rd::lobby {
namespace {

std::string utf16_to_utf8(const game::Ram &ram, std::uint32_t address, std::size_t units) {
    std::string text;
    for (std::size_t i = 0u; i < units; ++i) {
        std::uint32_t c = ram.load16(address + static_cast<std::uint32_t>(i * 2u));
        if (c == 0u) break;
        if (c >= 0xFF01u && c <= 0xFF5Eu) c -= 0xFEE0u;
        if (c == 0x3000u) c = ' ';
        if (c < 0x80u) {
            text += static_cast<char>(c);
        } else if (c < 0x800u) {
            text += static_cast<char>(0xC0u | (c >> 6u));
            text += static_cast<char>(0x80u | (c & 0x3Fu));
        } else {
            text += static_cast<char>(0xE0u | (c >> 12u));
            text += static_cast<char>(0x80u | ((c >> 6u) & 0x3Fu));
            text += static_cast<char>(0x80u | (c & 0x3Fu));
        }
    }
    return text;
}

bool printable(const std::string &text) {
    return !text.empty() &&
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20u && c != 0x7Fu; });
}

std::string ascii_text(const game::Ram &ram, std::uint32_t address, std::size_t max_bytes) {
    std::string text;
    for (std::size_t i = 0u; i < max_bytes; ++i) {
        if (!ram.contains(address + static_cast<std::uint32_t>(i), 1u)) break;
        const std::uint8_t c = ram.load8(address + static_cast<std::uint32_t>(i));
        if (c == 0u || c < 0x20u || c >= 0x7Fu) break;
        text.push_back(static_cast<char>(c));
    }
    return text;
}

} // namespace

std::uint32_t hunter_rank(const game::Ram &ram) {
    // The hunter rank is a byte in the character block; an HR4 save has 4
    // there and an early save has 0. The field beside the name is village
    // progress, not the hunter rank.
    constexpr std::uint32_t kRankOffset = 0x8Du;
    const std::uint32_t at = game::kCharacter + kRankOffset;
    return ram.contains(at, 1u) ? ram.load8(at) : 0u;
}

void guild_card(const game::Ram &ram, const std::string &name, std::string &title, std::string &comment) {
    title.clear();
    comment.clear();
    if (name.empty() || !ram.contains(kGuildCardListBase, kGuildCardListBytes)) return;
    // The hall keeps the local hunter's own card first, then the received
    // cards. The name is at the record's start, the title 0x54 bytes later and
    // the comment at 0x6E (traced on a live hall dump).
    for (std::uint32_t at = kGuildCardListBase; at + 0x8Cu <= kGuildCardListBase + kGuildCardListBytes; at += 2u) {
        if (utf16_to_utf8(ram, at, 12u) != name) continue;
        const std::string found_title = utf16_to_utf8(ram, at + 0x54u, 13u);
        const std::string found_comment = utf16_to_utf8(ram, at + 0x6Eu, 24u);
        if (printable(found_title) && printable(found_comment)) {
            title = found_title;
            comment = found_comment;
            return;
        }
    }
}

void guild_stats(const game::Ram &ram, GuildStats &stats) {
    stats = GuildStats{};

    // The guild card lists the quest counts in this order.
    if (ram.contains(kQuestCountsBase, 5u * 2u)) {
        static const char *const labels[] = {"Village", "Low Rank", "High Rank", "Training", "Hot Spring"};
        for (std::size_t i = 0u; i < 5u; ++i)
            stats.quests.push_back(
                StatCount{labels[i], ram.load16(kQuestCountsBase + static_cast<std::uint32_t>(i * 2u))});
    }

    // Round to minutes: the card shows hours and minutes, and a per-second
    // change would otherwise make every report differ from the last one.
    if (ram.contains(kPlayTime, 4u)) stats.play_seconds = (ram.load32(kPlayTime) / 60u) * 60u;

    if (ram.contains(kWeaponUsageBase, 13u * 2u)) {
        for (std::uint8_t kind = 5u; kind <= 17u; ++kind) {
            if (kind == 10u) continue; // the game's unused weapon kind
            const std::uint16_t count = ram.load16(kWeaponUsageBase + static_cast<std::uint32_t>(kind - 5u) * 2u);
            if (count == 0u) continue;
            const game::EquipmentKind *info = game::equipment_kind(kind);
            stats.weapons.push_back(StatCount{info != nullptr ? info->label : "Weapon", count});
        }
        std::sort(stats.weapons.begin(), stats.weapons.end(), [](const StatCount &a, const StatCount &b) {
            return a.count != b.count ? a.count > b.count : a.label < b.label;
        });
    }

    // The guild card's FS level is the sum of the five felyne levels.
    if (ram.contains(kFelyneLevelsBase + 4u * kFelyneLevelStride, 1u)) {
        for (std::uint32_t i = 0u; i < 5u; ++i) stats.fs_level += ram.load8(kFelyneLevelsBase + i * kFelyneLevelStride);
    }

    // The card's award count is not the non-zero bytes at 0x09FA8344; that
    // block is the monster log. Leave awards at zero until the real award
    // flags are traced, rather than showing a made-up count.

    // The recent-quest block carries the quest titles as ASCII strings. Skip
    // the short player names and the card greeting between them.
    if (ram.contains(kRecentQuestsBase, kRecentQuestsBytes)) {
        for (std::uint32_t at = kRecentQuestsBase; at + 8u < kRecentQuestsBase + kRecentQuestsBytes; ++at) {
            if (!ram.contains(at, 1u) || ram.load8(at) < 0x20u || ram.load8(at) == 0x7Fu) continue;
            const bool start = at == kRecentQuestsBase || ram.load8(at - 1u) < 0x20u || ram.load8(at - 1u) == 0x7Fu;
            if (!start) continue;
            const std::string text = ascii_text(ram, at, 64u);
            if (text.size() < 8u || text.find("gathering") != std::string::npos) continue;
            if (std::find(stats.recent_quests.begin(), stats.recent_quests.end(), text) == stats.recent_quests.end())
                stats.recent_quests.push_back(text);
        }
        std::reverse(stats.recent_quests.begin(), stats.recent_quests.end());
    }
}

std::vector<std::uint16_t> posted_quest_ids(const game::Ram &ram) {
    std::vector<std::uint16_t> ids;
    if (!ram.contains(kQuestBoardBase, kQuestBoardBytes)) return ids;
    for (std::uint32_t at = kQuestBoardBase; at + 2u <= kQuestBoardBase + kQuestBoardBytes && ids.size() < 4u;
        at += 2u) {
        const std::uint16_t id = ram.load16(at);
        if (id < 10101u || id > 10899u) continue;
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
        ids.push_back(id);
    }
    return ids;
}

std::uint16_t latest_quest_id(const game::Ram &ram) {
    if (!ram.contains(kQuestBoardBase, kQuestBoardBytes)) return 0u;
    std::vector<std::uint16_t> ids;
    for (std::uint32_t at = kQuestBoardBase; at + 2u <= kQuestBoardBase + kQuestBoardBytes; at += 2u) {
        const std::uint16_t id = ram.load16(at);
        if (id < 10101u || id > 10899u) continue;
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    return ids.empty() ? 0u : ids.back();
}

} // namespace mhp3rd::lobby
