#include "game/quest_builder.hpp"

#include "game/quest_file.hpp"

#include <algorithm>
#include <cstring>

namespace mhp3rd::game {
namespace {

// The payload's header (docs/QUESTS.md, "The payload").
constexpr std::size_t kHeader = 0x94u;
constexpr std::size_t kRecordSize = 0x48u;
constexpr std::size_t kLanguages = 7u;
constexpr std::size_t kSpawnSize = 0x30u;
constexpr std::size_t kMaxMonsters = 5u;
// The disc's hunts draw up to 35 lines for their main reward (0x23).
constexpr std::size_t kMostRewardLines = 40u;
constexpr std::uint32_t kFramesPerMinute = 1800u;
// The record's quest type (+0): a hunt; its objective count (+0x27) and kind
// (+0x28, 1: hunt or slay).
constexpr std::uint8_t kHunt = 0x04u;
// An empty small monster index: its second word must be 0x9999, or the
// large monsters do not come (found by building quests both ways).
constexpr std::uint32_t kNoSmallMonsters = 0x9999u;
// The small monsters' stats (+0x58, +0x5C), and the words a hunt has from
// +0x70 (the most common values over the disc's quests).
constexpr std::uint32_t kSmallMonsterStats = 0x10000064u;
constexpr std::uint32_t kSmallMonsterLevels = 9u;

class Writer {
public:
    std::vector<std::uint8_t> bytes;

    std::size_t here() const { return bytes.size(); }
    void zeros(std::size_t n) { bytes.insert(bytes.end(), n, 0u); }
    void align(std::size_t n) { zeros((n - bytes.size() % n) % n); }
    void u8(std::size_t at, std::uint8_t v) { bytes[at] = v; }
    void u16(std::size_t at, std::uint16_t v) { std::memcpy(bytes.data() + at, &v, 2u); }
    void u32(std::size_t at, std::uint32_t v) { std::memcpy(bytes.data() + at, &v, 4u); }
    void f32(std::size_t at, float v) { std::memcpy(bytes.data() + at, &v, 4u); }
    std::size_t put32(std::uint32_t v) {
        const std::size_t at = here();
        zeros(4u);
        u32(at, v);
        return at;
    }
    std::size_t put16(std::uint16_t v) {
        const std::size_t at = here();
        zeros(2u);
        u16(at, v);
        return at;
    }
    std::size_t text(const std::string &s) {
        const std::size_t at = here();
        bytes.insert(bytes.end(), s.begin(), s.end());
        zeros(1u);
        align(4u);
        return at;
    }
};

} // namespace

std::optional<std::vector<std::uint8_t>> build_quest(const QuestSpec &spec, std::string *why) {
    const auto fail = [why](const char *reason) -> std::optional<std::vector<std::uint8_t>> {
        if (why != nullptr) *why = reason;
        return std::nullopt;
    };
    if (!is_mod_quest(spec.id)) return fail("its id is not a mod quest's (62000 to 65535)");
    if (spec.monsters.empty() || spec.monsters.size() > kMaxMonsters) return fail("it needs 1 to 5 large monsters");
    if (spec.rewards.size() > kMostRewardLines) return fail("it has more reward lines than the game draws");
    for (const QuestRewardLine &line : spec.rewards) {
        unsigned total = 0;
        for (const QuestRewardItem &i : line) total += i.chance;
        if (line.empty() || total != 100u || std::any_of(line.begin(), line.end(), [](const QuestRewardItem &i) {
                return i.item == 0u || i.count == 0u;
            }))
            return fail("a reward line's chances do not add up to 100, or it names no item");
    }

    Writer w;
    w.zeros(kHeader);
    // The record, as the counter's lists hold it.
    const std::size_t record = w.here();
    w.zeros(kRecordSize);
    w.u8(record, kHunt);
    w.u32(record + 0x04u, spec.fee);
    w.u32(record + 0x08u, spec.reward);
    w.u32(record + 0x0Cu, spec.reward / 3u); // a sub-reward, as the game's hunts have a third
    w.u32(record + 0x10u, spec.minutes * kFramesPerMinute);
    w.u16(record + 0x1Cu, spec.id);
    w.u16(record + 0x1Eu, spec.stars);
    w.u8(record + 0x20u, spec.map);
    w.u8(record + 0x27u, 1u);
    w.u8(record + 0x28u, 1u);
    w.u16(record + 0x2Cu, spec.target);
    w.u16(record + 0x2Eu, spec.count);
    for (std::size_t i = 0; i < spec.monsters.size(); ++i)
        w.u16(record + 0x3Cu + 2u * i, spec.monsters[i].kind);
    // Its texts, one set for every language.
    const QuestTexts &t = spec.texts;
    std::vector<std::size_t> texts;
    for (const std::string *s : {&t.name, &t.objective, &t.failure, &t.description, &t.monsters, &t.client})
        texts.push_back(w.text(*s));
    const std::size_t set = w.here();
    for (const std::size_t at : texts) w.put32(static_cast<std::uint32_t>(at));
    const std::size_t languages = w.here();
    for (std::size_t i = 0; i < kLanguages; ++i) w.put32(static_cast<std::uint32_t>(set));
    w.u32(record + 0x18u, static_cast<std::uint32_t>(languages));

    // The main reward: a group header (kind 0, its line count; the word after
    // it not traced, 0 here), the end of the groups (0xFF), then each line's
    // items (chance, count, item) ended by 0xFFFFFFFF.
    const std::size_t rewards = w.here();
    if (!spec.rewards.empty()) {
        w.put32(static_cast<std::uint32_t>(spec.rewards.size()) << 8u);
        w.put32(0u);
    }
    w.put32(0xFFu);
    w.zeros(4u);
    for (const QuestRewardLine &line : spec.rewards) {
        for (const QuestRewardItem &i : line) {
            w.bytes.push_back(i.chance);
            w.bytes.push_back(i.count);
            w.put16(i.item);
        }
        w.put32(0xFFFFFFFFu);
    }
    // No supplies or gathering yet: each table as the game reads it empty.
    const std::size_t supplies = w.here();
    w.zeros(8u);
    const std::size_t gathering = w.put16(0xFFFFu);
    w.zeros(10u);
    const std::size_t small_index = w.put32(0u);
    w.put32(kNoSmallMonsters);
    w.zeros(16u);
    const std::size_t small_waves = w.here();
    for (int i = 0; i < 3; ++i) w.put32(static_cast<std::uint32_t>(small_index));
    w.put32(0u);
    const std::size_t small_extra = w.put16(0xFFFFu);
    w.zeros(kSpawnSize - 2u);
    // The large monsters: where each starts, the kinds to load, and the group.
    const std::size_t spawns = w.here();
    for (const QuestMonster &m : spec.monsters) {
        const std::size_t at = w.here();
        w.zeros(kSpawnSize);
        w.u32(at, m.kind);
        w.u16(at + 4u, 1u);
        w.u16(at + 6u, m.area);
        w.f32(at + 0x10u, m.x);
        w.f32(at + 0x14u, m.y);
        w.f32(at + 0x18u, m.z);
    }
    w.put16(0xFFFFu);
    w.zeros(kSpawnSize - 2u);
    std::vector<std::uint16_t> kinds;
    for (const QuestMonster &m : spec.monsters)
        if (std::find(kinds.begin(), kinds.end(), m.kind) == kinds.end()) kinds.push_back(m.kind);
    const std::size_t loads = w.here();
    for (const std::uint16_t kind : kinds) {
        w.put16(kind);
        w.put16(0u);
    }
    for (int i = 0; i < 3; ++i) w.put32(0xFFFFFFFFu);
    const std::size_t large = w.put32(1u);
    w.put32(0u);
    w.put32(static_cast<std::uint32_t>(loads));
    w.put32(static_cast<std::uint32_t>(spawns));
    w.zeros(16u);
    const std::size_t intruders = w.here();
    for (const std::uint8_t b : {0xFFu, 0xFFu, 0x00u, 0xFFu, 0xFFu, 0xFFu, 0x00u, 0x00u}) w.bytes.push_back(b);
    w.zeros(44u);
    w.align(16u);

    // The header: where each table is, and each large monster's stats.
    w.u32(0x00u, static_cast<std::uint32_t>(record));
    std::memcpy(w.bytes.data() + 4u, "v005", 4u);
    w.u32(0x08u, static_cast<std::uint32_t>(rewards));
    w.u32(0x1Cu, static_cast<std::uint32_t>(gathering));
    w.u32(0x20u, static_cast<std::uint32_t>(large));
    w.u32(0x24u, static_cast<std::uint32_t>(small_waves));
    w.u32(0x28u, static_cast<std::uint32_t>(small_extra));
    w.u32(0x2Cu, static_cast<std::uint32_t>(intruders));
    for (std::size_t i = 0; i < spec.monsters.size(); ++i) {
        const QuestMonster &m = spec.monsters[i];
        // health level (byte 3), size in percent (low half); attack level
        // (byte 0), then two levels not traced yet, at a hunt's usual values.
        w.u32(0x30u + 8u * i, (std::uint32_t{m.health} << 24u) | m.size);
        w.u32(0x34u + 8u * i, std::uint32_t{m.attack} | (0x13u << 8u) | (0x03u << 16u));
    }
    w.u32(0x58u, kSmallMonsterStats);
    w.u32(0x5Cu, kSmallMonsterLevels);
    w.u32(0x60u, static_cast<std::uint32_t>(supplies));
    w.u32(0x70u, 0x01000000u);
    w.u32(0x78u, 0x02000000u);
    w.u32(0x7Cu, 150u);
    w.u32(0x80u, 15u);
    w.u32(0x90u, 0x00010100u);
    if (w.bytes.size() > kQuestMaxPayload) return fail("its texts make it larger than the game reads");
    return std::move(w.bytes);
}

} // namespace mhp3rd::game
