#include "lobby/diag.hpp"

#include "game/game_data.hpp"
#include "game/guest_ram.hpp"
#include "lobby/quests.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mhp3rd::lobby {
namespace {

// The live name/guild-card/board data and the save copy the game keeps while
// loading sit in this window; the scan is limited so a diagnostic dump does not
// read all 64 MiB of guest RAM.
constexpr std::uint32_t kScanBase = 0x09500000u;
constexpr std::uint32_t kScanBytes = 0x00E00000u; // 0x09500000..0x0A300000
constexpr std::size_t kContextBefore = 0x100u;
constexpr std::size_t kContextAfter = 0x200u;
constexpr std::size_t kMaxHits = 32u;

std::string hex_byte(std::uint8_t value) {
    std::ostringstream text;
    text << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(value);
    return text.str();
}

std::string utf16_text(const game::Ram &ram, std::uint32_t address, std::size_t units) {
    std::string text;
    for (std::size_t i = 0u; i < units; ++i) {
        if (!ram.contains(address + static_cast<std::uint32_t>(i * 2u), 2u)) break;
        std::uint32_t c = ram.load16(address + static_cast<std::uint32_t>(i * 2u));
        if (c == 0u) break;
        if (c >= 0xFF01u && c <= 0xFF5Eu) c -= 0xFEE0u;
        if (c == 0x3000u) c = ' ';
        if (c < 0x80u) {
            text.push_back(static_cast<char>(std::isprint(static_cast<unsigned char>(c)) ? c : '.'));
        } else if (c < 0x800u) {
            text.push_back(static_cast<char>(0xC0u | (c >> 6u)));
            text.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        } else {
            text.push_back(static_cast<char>(0xE0u | (c >> 12u)));
            text.push_back(static_cast<char>(0x80u | ((c >> 6u) & 0x3Fu)));
            text.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        }
    }
    return text;
}

bool utf16_equals(const game::Ram &ram, std::uint32_t address, const std::string &text) {
    for (std::size_t i = 0u; i < text.size(); ++i) {
        if (!ram.contains(address + static_cast<std::uint32_t>(i * 2u), 2u)) return false;
        std::uint32_t c = ram.load16(address + static_cast<std::uint32_t>(i * 2u));
        if (c >= 0xFF01u && c <= 0xFF5Eu) c -= 0xFEE0u;
        if (c == 0x3000u) c = ' ';
        if (c != static_cast<unsigned char>(text[i])) return false;
    }
    return true;
}

std::string ascii_run(const game::Ram &ram, std::uint32_t address, std::size_t max_bytes) {
    std::string text;
    for (std::size_t i = 0u; i < max_bytes; ++i) {
        if (!ram.contains(address + static_cast<std::uint32_t>(i), 1u)) break;
        const std::uint8_t c = ram.load8(address + static_cast<std::uint32_t>(i));
        if (c == 0u) break;
        if (c < 0x20u || c == 0x7Fu) break;
        text.push_back(static_cast<char>(c));
    }
    return text;
}

void write_range(const std::filesystem::path &path, const game::Ram &ram, std::uint32_t base, std::size_t bytes) {
    std::vector<std::uint8_t> data;
    data.reserve(bytes);
    for (std::size_t i = 0u; i < bytes; ++i) {
        if (!ram.contains(base + static_cast<std::uint32_t>(i), 1u)) break;
        data.push_back(ram.load8(base + static_cast<std::uint32_t>(i)));
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

void hex_dump(std::ostream &out, const game::Ram &ram, std::uint32_t base, std::size_t bytes) {
    for (std::size_t offset = 0u; offset < bytes; offset += 16u) {
        std::ostringstream line;
        line << "0x" << std::hex << std::setw(8) << std::setfill('0') << (base + offset) << ": ";
        std::string ascii;
        for (std::size_t i = 0u; i < 16u; ++i) {
            if (offset + i >= bytes || !ram.contains(base + static_cast<std::uint32_t>(offset + i), 1u)) {
                line << "   ";
                ascii.push_back(' ');
                continue;
            }
            const std::uint8_t c = ram.load8(base + static_cast<std::uint32_t>(offset + i));
            line << hex_byte(c) << ' ';
            ascii.push_back(std::isprint(c) ? static_cast<char>(c) : '.');
        }
        out << line.str() << " " << ascii << "\n";
    }
}

void context(std::ostream &out, const game::Ram &ram, std::uint32_t hit) {
    const std::uint32_t start = hit - std::min<std::uint32_t>(hit - kScanBase, kContextBefore);
    const std::uint32_t end = std::min(kScanBase + kScanBytes, hit + static_cast<std::uint32_t>(kContextAfter));
    hex_dump(out, ram, start, end - start);
    out << "  UTF-16 runs:\n";
    for (std::uint32_t at = start; at + 4u < end; at += 2u) {
        const std::string text = utf16_text(ram, at, 32u);
        if (text.size() >= 3u && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20u; }))
            out << "    +0x" << std::hex << (at - start) << ": \"" << text << "\"\n";
    }
    out << "  ASCII runs:\n";
    for (std::uint32_t at = start; at + 4u < end; ++at) {
        const std::string text = ascii_run(ram, at, 64u);
        if (text.size() >= 4u) out << "    +0x" << std::hex << (at - start) << ": \"" << text << "\"\n";
    }
}

std::size_t name_hits(std::ostream &out, const game::Ram &ram, const std::string &name) {
    if (name.empty()) return 0u;
    std::size_t hits = 0u;
    // The game keeps ASCII names in their fullwidth forms; utf16_equals folds
    // those back, and the ASCII pass catches copies the text block kept as is.
    for (std::uint32_t at = kScanBase; at + 2u < kScanBase + kScanBytes && hits < kMaxHits; at += 2u) {
        if (!utf16_equals(ram, at, name)) continue;
        out << "=== UTF-16 name hit at 0x" << std::hex << at << " ===\n";
        context(out, ram, at);
        ++hits;
    }
    for (std::uint32_t at = kScanBase; at + name.size() < kScanBase + kScanBytes && hits < kMaxHits; ++at) {
        bool match = true;
        for (std::size_t i = 0u; i < name.size(); ++i) {
            if (!ram.contains(at + static_cast<std::uint32_t>(i), 1u) ||
                ram.load8(at + static_cast<std::uint32_t>(i)) != static_cast<std::uint8_t>(name[i])) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        out << "=== ASCII name hit at 0x" << std::hex << at << " ===\n";
        context(out, ram, at);
        ++hits;
    }
    return hits;
}

std::size_t text_hits(std::ostream &out, const game::Ram &ram, const std::string &needle) {
    if (needle.empty()) return 0u;
    std::size_t hits = 0u;
    for (std::uint32_t at = kScanBase; at + needle.size() < kScanBase + kScanBytes && hits < kMaxHits; ++at) {
        bool match = true;
        for (std::size_t i = 0u; i < needle.size(); ++i) {
            if (!ram.contains(at + static_cast<std::uint32_t>(i), 1u) ||
                ram.load8(at + static_cast<std::uint32_t>(i)) != static_cast<std::uint8_t>(needle[i])) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        out << "=== ASCII text hit at 0x" << std::hex << at << " ===\n";
        context(out, ram, at);
        ++hits;
    }
    for (std::uint32_t at = kScanBase; at + 2u * needle.size() < kScanBase + kScanBytes && hits < kMaxHits; at += 2u) {
        if (!utf16_equals(ram, at, needle)) continue;
        out << "=== UTF-16 text hit at 0x" << std::hex << at << " ===\n";
        context(out, ram, at);
        ++hits;
    }
    return hits;
}

void small_values(std::ostream &out, const game::Ram &ram, std::uint32_t base, std::size_t bytes) {
    for (std::size_t offset = 0u; offset < bytes; ++offset) {
        if (!ram.contains(base + static_cast<std::uint32_t>(offset), 1u)) break;
        const std::uint8_t byte = ram.load8(base + static_cast<std::uint32_t>(offset));
        if (byte > 0u && byte <= 100u)
            out << "  +0x" << std::hex << offset << " u8 " << std::dec << static_cast<unsigned>(byte) << "\n";
        if (offset + 2u > bytes || !ram.contains(base + static_cast<std::uint32_t>(offset), 2u)) continue;
        const std::uint16_t value = ram.load16(base + static_cast<std::uint32_t>(offset));
        if (value > 0u && value <= 1000u) out << "  +0x" << std::hex << offset << " u16 " << std::dec << value << "\n";
        if (offset + 4u > bytes || !ram.contains(base + static_cast<std::uint32_t>(offset), 4u)) continue;
        const std::uint32_t wide = ram.load32(base + static_cast<std::uint32_t>(offset));
        if (wide > 0u && wide <= 1000u) out << "  +0x" << std::hex << offset << " u32 " << std::dec << wide << "\n";
    }
}

void write_summary(const std::filesystem::path &path, const game::Ram &ram, const Snapshot &snapshot) {
    std::ofstream out(path);
    out << "Yakumo lobby diagnostic dump\n";
    out << "name: " << snapshot.name << "\n";
    out << "hall: " << snapshot.hall << "\n";
    out << "reported hr: " << snapshot.hr << "\n";
    out << "reported weapon: " << (snapshot.weapon ? snapshot.weapon->name : "") << " / "
        << (snapshot.weapon ? snapshot.weapon->type : "") << "\n";
    out << "character block (kCharacter = 0x" << std::hex << game::kCharacter << std::dec << ")\n";
    hex_dump(out, ram, game::kCharacter, 0x200u);
    out << "  character+0x8D u8 (hunter rank candidate): "
        << static_cast<unsigned>(ram.contains(game::kCharacter + 0x8Du, 1u) ? ram.load8(game::kCharacter + 0x8Du) : 0u)
        << "\n";
    out << "\nsmall values in the character block:\n";
    small_values(out, ram, game::kCharacter, 0x200u);
    out << "\n0x09FAC8B0..0x09FAC8F0:\n";
    hex_dump(out, ram, 0x09FAC8B0u, 0x40u);
    out << "  0x09FAC8C5 u8: " << static_cast<unsigned>(ram.contains(0x09FAC8C5u, 1u) ? ram.load8(0x09FAC8C5u) : 0u)
        << "\n";
    out << "  0x09FAC8D4 u32 (money): " << (ram.contains(0x09FAC8D4u, 4u) ? ram.load32(0x09FAC8D4u) : 0u) << "\n";
    out << "\nsave copy around 0x095B1BC8:\n";
    hex_dump(out, ram, 0x095B1B80u, 0x180u);

    out << "\nhunter records:\n";
    if (ram.contains(0x08AB3640u, 4u)) {
        const std::uint32_t pointer = ram.load32(0x08AB3640u);
        out << "  pointer: 0x" << std::hex << pointer << std::dec << "\n";
        hex_dump(out, ram, pointer + 0x30u, 4u * 252u);
        for (std::size_t i = 0u; i < 4u; ++i) {
            const std::uint32_t at = pointer + 0x30u + static_cast<std::uint32_t>(i * 252u);
            out << "  record " << i << " strings:";
            for (std::size_t offset = 0u; offset + 2u < 252u; offset += 2u) {
                const std::string text = utf16_text(ram, at + static_cast<std::uint32_t>(offset), 32u);
                if (text.size() >= 3u) out << " +0x" << std::hex << offset << "=\"" << text << "\"";
            }
            out << "\n";
        }
    } else {
        out << "  pointer not in memory\n";
    }

    out << "\nquest board windows:\n";
    for (const std::uint32_t base : {kQuestBoardBase, 0x0A2E6000u}) {
        out << "  window 0x" << std::hex << base << std::dec << ":\n";
        hex_dump(out, ram, base, 0x800u);
        out << "  ids 10101..10899:";
        for (std::uint32_t at = base; at + 2u < base + 0x800u; at += 2u) {
            if (!ram.contains(at, 2u)) break;
            const std::uint16_t id = ram.load16(at);
            if (id >= 10101u && id <= 10899u) out << " 0x" << std::hex << at << "=" << std::dec << id;
        }
        out << "\n";
    }

    std::ostringstream names;
    const std::size_t found_names = name_hits(names, ram, snapshot.name);
    std::ofstream name_file(path.parent_path() / "name_hits.txt");
    name_file << names.str();
    const char *needle_env = std::getenv("MHP3RD_LOBBY_DUMP_TEXT");
    const std::string needle = needle_env != nullptr && *needle_env != '\0' ? needle_env : "Zinogre";
    std::ostringstream texts;
    const std::size_t found_texts = text_hits(texts, ram, needle);
    std::ofstream text_file(path.parent_path() / "text_hits.txt");
    text_file << texts.str();
    out << "\nguild card list candidate at 0x09FA8DBA:\n";
    hex_dump(out, ram, 0x09FA8DBAu, 0xA0u);
    out << "  name: \"" << utf16_text(ram, 0x09FA8DBAu, 12u) << "\" title: \""
        << utf16_text(ram, 0x09FA8DBAu + 0x54u, 13u) << "\" comment: \"" << utf16_text(ram, 0x09FA8DBAu + 0x6Eu, 24u)
        << "\"\n";
    GuildStats card;
    guild_stats(ram, card);
    out << "  play: " << card.play_seconds << "s fs: " << card.fs_level << " awards: " << card.awards << "\n";
    for (const StatCount &quest : card.quests) out << "  quests " << quest.label << ": " << quest.count << "\n";
    for (const StatCount &weapon : card.weapons) out << "  weapon " << weapon.label << ": " << weapon.count << "\n";
    for (const std::string &recent : card.recent_quests) out << "  recent: " << recent << "\n";
    out << "\nsearch: " << found_names << " name hit(s), " << found_texts << " \"" << needle << "\" hit(s)\n";
    out << "files: character.bin, hunter_records.bin, save_copy.bin, questboard_9f.bin, questboard_a2.bin, "
           "name_hits.txt, text_hits.txt\n";
}

} // namespace

void maybe_dump(const game::Ram &ram, const Snapshot &snapshot) {
    const char *setting = std::getenv("MHP3RD_LOBBY_DUMP");
    if (setting == nullptr || *setting == '\0' || snapshot.hall.empty()) return;
    // The first dump is taken as the hall opens, before a quest is posted; the
    // next two, ten seconds apart, capture the board after it is posted.
    constexpr int kMaxDumps = 3;
    constexpr auto kDumpInterval = std::chrono::seconds(10);
    static std::string last_hall;
    static std::chrono::steady_clock::time_point last_dump{};
    static int dumps{};
    if (last_hall != snapshot.hall) {
        last_hall = snapshot.hall;
        dumps = 0;
        last_dump = {};
    }
    if (dumps >= kMaxDumps) return;
    const auto now = std::chrono::steady_clock::now();
    if (dumps > 0 && now - last_dump < kDumpInterval) return;
    last_dump = now;
    ++dumps;

    std::filesystem::path directory(setting);
    if (directory == "1") directory = "logs";
    directory /= "lobby-dump-" + std::to_string(static_cast<long long>(std::time(nullptr)));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cout << "[lobby] cannot create the diagnostic dump folder " << directory.string() << ": "
                  << error.message() << std::endl;
        return;
    }
    std::cout << "[lobby] diagnostic dump: scanning hall memory in " << directory.string() << std::endl;
    write_range(directory / "character.bin", ram, game::kCharacter, 0x400u);
    if (ram.contains(0x08AB3640u, 4u)) {
        const std::uint32_t pointer = ram.load32(0x08AB3640u);
        write_range(directory / "hunter_records.bin", ram, pointer + 0x30u, 4u * 252u);
    }
    write_range(directory / "save_copy.bin", ram, 0x095B0000u, 0x10000u);
    write_range(directory / "questboard_9f.bin", ram, kQuestBoardBase, 0x2000u);
    write_range(directory / "questboard_a2.bin", ram, 0x0A2E6000u, 0x2000u);
    write_summary(directory / "summary.txt", ram, snapshot);
    std::cout << "[lobby] diagnostic dump written to " << directory.string() << std::endl;
}

} // namespace mhp3rd::lobby
