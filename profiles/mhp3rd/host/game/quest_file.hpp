#pragma once

// The game's quest files: one DATA.BIN entry per quest, encrypted, which the
// quest overlay decrypts and checks before it plays the quest. Traced on
// NPJB-40001 (docs/QUESTS.md):
//
// - The file starts with four 16-bit seeds. Each seed starts a key (a seed of
//   0 starts it from its multiplier); every 16-bit word after the seeds is
//   XORed with the next value of the key its position chooses, key i for the
//   word at byte offset p being ((p >> 1) & 3), stepped as
//   key = key * multiplier[i] % modulus[i].
// - Bytes 8 to 31 decrypt to the payload's size (u32) and the SHA-1 of the
//   payload followed by a 16-byte salt; the payload follows, the keys running
//   on from the header.
// - The multipliers, moduli and salt are the executable's (0x089D5CE0,
//   0x089D5CD0, 0x0896BB48): they are read from guest memory, never stored here.
//
// The payload begins with the offset of the quest's record, the same record
// the counter's lists hold: the fee at +4, the reward at +8, the time limit in
// frames at +0x10, the id (u16) at +0x1C, the stars at +0x1E, the map at
// +0x20, and the name and texts from +0x48.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mhp3rd::game {

class Ram;

inline constexpr std::uint32_t kQuestMultipliers = 0x089D5CE0u;
inline constexpr std::uint32_t kQuestModuli = 0x089D5CD0u;
inline constexpr std::uint32_t kQuestSalt = 0x0896BB48u;
inline constexpr std::size_t kQuestHeader = 32u;      // seeds, then the encrypted size and digest
inline constexpr std::size_t kQuestMaxPayload = 24576u; // the buffer the quest overlay reads into

struct QuestKeys {
    std::array<std::uint32_t, 4> multipliers{};
    std::array<std::uint32_t, 4> moduli{};
    std::array<std::uint8_t, 16> salt{};
};

// The keys as the executable holds them; nothing when one of them is 0, which
// would be another executable.
[[nodiscard]] std::optional<QuestKeys> read_quest_keys(const Ram &ram);

// SHA-1 of `bytes`.
[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> bytes);

// The payload of an encrypted quest file, or nothing (with the reason in
// `why`) when it is too short, its size is out of range or its digest does
// not match.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> decrypt_quest(
    std::span<const std::uint8_t> file, const QuestKeys &keys, std::string *why = nullptr);

// An encrypted quest file holding `payload` (with a zero byte added when its
// size is odd), which the game accepts. The seeds pick the key stream; any
// non-zero values do.
[[nodiscard]] std::vector<std::uint8_t> encrypt_quest(std::span<const std::uint8_t> payload, const QuestKeys &keys,
    std::array<std::uint16_t, 4> seeds = {0x1F2Eu, 0x3D4Cu, 0x5B6Au, 0x7988u});

// The parts of a payload's quest record this host reads.
struct QuestRecord {
    std::uint32_t offset{}; // of the record in the payload
    std::uint32_t fee{};
    std::uint32_t reward{};
    std::uint32_t frames{}; // the time limit, at 30 a second
    std::uint16_t id{};
    std::uint16_t stars{};
    std::uint8_t map{};
    std::string name;
};
[[nodiscard]] std::optional<QuestRecord> read_quest_record(std::span<const std::uint8_t> payload);

// Quest ids mods use: 62000 to 65535. Ids above 59999 are the game's event
// quests, which it lists among the downloaded ones; its own run from 60001
// to 60053 and from 61001 to 61011 (docs/QUESTS.md), and its id-to-file code
// has no branch for any of them.
inline constexpr std::uint16_t kFirstModQuest = 62000u;
[[nodiscard]] constexpr bool is_mod_quest(std::uint32_t id) noexcept {
    return id >= kFirstModQuest && id <= 0xFFFFu;
}

} // namespace mhp3rd::game
