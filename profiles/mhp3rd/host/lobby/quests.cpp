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

} // namespace

std::uint32_t hunter_rank(const game::Ram &ram) {
    // The rank sits beside the money and the two point balances. The field
    // beside the character's name is not it: a rank 4 hunter reads 7 there.
    constexpr std::uint32_t kRank = 0x09FAC8C5u;
    return ram.contains(kRank, 1u) ? ram.load8(kRank) : 0u;
}

void guild_card(const game::Ram &ram, const std::string &name, std::string &title, std::string &comment) {
    title.clear();
    comment.clear();
    if (name.empty() || !ram.contains(kQuestBoardBase, kQuestBoardBytes)) return;
    for (std::uint32_t at = kQuestBoardBase; at + 0x80u <= kQuestBoardBase + kQuestBoardBytes; at += 2u) {
        if (utf16_to_utf8(ram, at, 12u) != name) continue;
        const std::string found_title = utf16_to_utf8(ram, at + 0x1Au, 13u);
        const std::string found_comment = utf16_to_utf8(ram, at + 0x34u, 24u);
        if (printable(found_title) && printable(found_comment)) {
            title = found_title;
            comment = found_comment;
            return;
        }
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

} // namespace mhp3rd::lobby
