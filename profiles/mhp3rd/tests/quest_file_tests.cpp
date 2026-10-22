// The game's quest file cipher and record (game/quest_file.hpp) on made-up
// keys: SHA-1 against the standard's examples, a file encrypted by an
// independent implementation of the traced algorithm, round trips, damage
// that must be refused, and the record's fields. No game data.
//
//   mhp3rd_quest_file_tests
//   mhp3rd_quest_file_tests --decrypt <EBOOT.ELF> <quest file> [<payload out>]
//
// The second form reads the keys from a real executable (loaded at its
// address) and decrypts a quest file extracted from the disc, to check the
// cipher against the game's own files.
#include "game/guest_ram.hpp"
#include "game/quest_builder.hpp"
#include "game/quest_file.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace mhp3rd::game;
using Bytes = std::vector<std::uint8_t>;
int failures{};

void check(bool condition, const std::string &what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) ++failures;
}

Bytes hex(const std::string &text) {
    Bytes out;
    for (std::size_t i = 0; i + 1u < text.size(); i += 2u)
        out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2u), nullptr, 16)));
    return out;
}

std::string hex(const std::array<std::uint8_t, 20> &bytes) {
    std::string out;
    char digits[3];
    for (const std::uint8_t byte : bytes) {
        std::snprintf(digits, sizeof(digits), "%02x", byte);
        out += digits;
    }
    return out;
}

// Made-up keys, laid out in a buffer as the executable lays its own out.
QuestKeys made_up_keys(BufferRam &ram) {
    const std::uint32_t multipliers[4] = {5897u, 15859u, 29819u, 45953u};
    const std::uint32_t moduli[4] = {65437u, 65449u, 65479u, 65521u};
    for (std::uint32_t i = 0; i < 4u; ++i) {
        ram.store32(kQuestMultipliers + i * 4u, multipliers[i]);
        ram.store32(kQuestModuli + i * 4u, moduli[i]);
    }
    for (std::uint32_t i = 0; i < 16u; ++i) ram.store8(kQuestSalt + i, static_cast<std::uint8_t>(0x30u + i));
    return *read_quest_keys(ram);
}

Bytes made_up_payload() {
    Bytes payload = hex("0800000000000000");
    const std::string name = "Test quest";
    payload.insert(payload.end(), name.begin(), name.end());
    payload.insert(payload.end(), {0u, 0u});
    for (std::uint8_t i = 0; i < 20u; ++i) payload.push_back(i);
    return payload;
}

void test_sha1() {
    const std::string abc = "abc";
    check(hex(sha1({reinterpret_cast<const std::uint8_t *>(abc.data()), abc.size()})) ==
            "a9993e364706816aba3e25717850c26c9cd0d89d",
        "sha1: \"abc\"");
    check(hex(sha1({})) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1: the empty message");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    check(hex(sha1({reinterpret_cast<const std::uint8_t *>(two.data()), two.size()})) ==
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
        "sha1: a message over one block");
}

void test_cipher() {
    BufferRam ram(0x08800000u, 0x01800000u);
    check(!read_quest_keys(ram), "keys: none in empty memory");
    const QuestKeys keys = made_up_keys(ram);

    // Encrypted by a separate implementation of the algorithm traced in the
    // executable, with the same keys and seeds 1111, 2222, 3333, 4444.
    const Bytes reference = hex("1111222233334444bcb8b94fc1bd990129f5c5c6e1fda0b872d9d79de3e908ce9e4d5dcc3202885c498"
                                "d508ded9f86e0f0ae2b5706d690ea0a145c2d3ddcc86a757e3d2ddf0331fe");
    const Bytes payload = made_up_payload();
    const std::optional<Bytes> decrypted = decrypt_quest(reference, keys);
    check(decrypted && *decrypted == payload, "cipher: a file from the reference implementation decrypts");
    check(encrypt_quest(payload, keys, {0x1111u, 0x2222u, 0x3333u, 0x4444u}) == reference,
        "cipher: encrypting gives the reference implementation's bytes");

    const Bytes file = encrypt_quest(payload, keys);
    check(file.size() == kQuestHeader + payload.size() && decrypt_quest(file, keys) == payload,
        "cipher: a round trip with the default seeds");
    Bytes odd = payload;
    odd.push_back(0x7Fu);
    const std::optional<Bytes> padded = decrypt_quest(encrypt_quest(odd, keys), keys);
    check(padded && padded->size() == odd.size() + 1u && padded->back() == 0u && (*padded)[odd.size() - 1u] == 0x7Fu,
        "cipher: an odd payload gets a zero byte");

    std::string why;
    Bytes damaged = file;
    damaged[kQuestHeader + 3u] ^= 0x01u;
    check(!decrypt_quest(damaged, keys, &why) && why.find("digest") != std::string::npos,
        "cipher: a changed byte is refused by the digest");
    Bytes cut(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(file.size() - 2u));
    check(!decrypt_quest(cut, keys, &why) && why.find("size") != std::string::npos,
        "cipher: a file shorter than its size is refused");
    check(!decrypt_quest(Bytes(10u, 0u), keys, &why) && why.find("header") != std::string::npos,
        "cipher: a file shorter than the header is refused");
    QuestKeys other = keys;
    other.salt[0] ^= 1u;
    check(!decrypt_quest(file, other), "cipher: another salt is refused");
}

void test_record() {
    Bytes payload(0x100u, 0u);
    payload[0] = 0x10u; // the record at 0x10
    const auto put32 = [&payload](std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4u; ++i) payload[at + i] = static_cast<std::uint8_t>(value >> (8u * i));
    };
    put32(0x10u + 4u, 200u);
    put32(0x10u + 8u, 1200u);
    put32(0x10u + 0x10u, 90000u);
    payload[0x10u + 0x1Cu] = 0x31u; // 62001
    payload[0x10u + 0x1Du] = 0xF2u;
    payload[0x10u + 0x1Eu] = 6u;
    payload[0x10u + 0x20u] = 12u;
    const std::string name = "Black Dragon";
    std::copy(name.begin(), name.end(), payload.begin() + 0x10 + 0x48);
    const std::optional<QuestRecord> record = read_quest_record(payload);
    check(record && record->offset == 0x10u && record->fee == 200u && record->reward == 1200u &&
            record->frames == 90000u && record->id == 62001u && record->stars == 6u && record->map == 12u &&
            record->name == name,
        "record: its fields");
    check(is_mod_quest(record->id) && !is_mod_quest(10840u) && !is_mod_quest(60001u) && !is_mod_quest(61011u) &&
            !is_mod_quest(70000u),
        "record: mod quest ids, above the game's event quests");
    payload[0] = 0xF0u;
    check(!read_quest_record(payload), "record: an offset with no room for the record is refused");
    check(!read_quest_record(Bytes{1u, 2u}), "record: a payload too short for an offset is refused");
}

// Checks the cipher on a real quest file with the executable's keys.
int decrypt_real(const char *eboot, const char *quest, const char *out) {
    std::ifstream elf(eboot, std::ios::binary);
    const Bytes image{std::istreambuf_iterator<char>(elf), std::istreambuf_iterator<char>()};
    if (image.size() < 0x100u) return 2;
    // The executable's single loadable segment, at its address.
    const auto load32 = [&image](std::size_t at) {
        return static_cast<std::uint32_t>(image[at] | image[at + 1u] << 8u | image[at + 2u] << 16u |
            static_cast<std::uint32_t>(image[at + 3u]) << 24u);
    };
    const std::uint32_t header = load32(0x1Cu);
    const std::uint32_t offset = load32(header + 4u), address = load32(header + 8u), size = load32(header + 16u);
    BufferRam ram(address, size);
    for (std::uint32_t i = 0; i < size && offset + i < image.size(); ++i) ram.store8(address + i, image[offset + i]);
    const std::optional<QuestKeys> keys = read_quest_keys(ram);
    if (!keys) {
        std::printf("no quest keys in that executable\n");
        return 1;
    }
    std::ifstream in(quest, std::ios::binary);
    const Bytes file{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::string why;
    const std::optional<Bytes> payload = decrypt_quest(file, *keys, &why);
    if (!payload) {
        std::printf("not decrypted: %s\n", why.c_str());
        return 1;
    }
    const std::optional<QuestRecord> record = read_quest_record(*payload);
    std::printf("decrypted %zu bytes: quest %u, %u stars, map %u, \"%s\"\n", payload->size(), record ? record->id : 0u,
        record ? record->stars : 0u, record ? record->map : 0u, record ? record->name.c_str() : "?");
    if (out != nullptr)
        std::ofstream(out, std::ios::binary)
            .write(reinterpret_cast<const char *>(payload->data()), static_cast<std::streamsize>(payload->size()));
    // Encrypted again with the file's own seeds, the game's bytes come back.
    std::array<std::uint16_t, 4> seeds{};
    for (std::size_t i = 0; i < 4u; ++i) seeds[i] = static_cast<std::uint16_t>(file[i * 2u] | file[i * 2u + 1u] << 8u);
    const Bytes again = encrypt_quest(*payload, *keys, seeds);
    return std::equal(again.begin(), again.end(), file.begin()) ? 0 : 3;
}

std::uint32_t word(const Bytes &b, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4u);
    return v;
}
std::uint16_t half(const Bytes &b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | b[at + 1u] << 8u);
}
float real(const Bytes &b, std::size_t at) {
    float v = 0;
    std::memcpy(&v, b.data() + at, 4u);
    return v;
}

void test_builder() {
    QuestSpec spec;
    spec.id = 62006u;
    spec.map = 8u;
    spec.stars = 8u;
    spec.minutes = 50u;
    spec.fee = 1000u;
    spec.reward = 9000u;
    spec.target = 75u;
    spec.monsters = {
        {75u, 1u, 1086.0f, 1.0f, 255.0f, 0x1Eu, 0x13u, 120u}, {58u, 2u, 0.0f, 0.0f, 100.0f, 0x10u, 0x10u, 100u}};
    spec.texts = {"Fatalis", "Hunt the Fatalis", "Reward Zero\nTime Over", "A black dragon", "Fatalis", "Guild"};
    std::string why;
    const std::optional<Bytes> built = build_quest(spec, &why);
    const std::optional<QuestRecord> record = built ? read_quest_record(*built) : std::nullopt;
    check(record && record->id == 62006u && record->map == 8u && record->stars == 8u && record->fee == 1000u &&
            record->reward == 9000u && record->frames == 50u * 1800u && record->name == "Fatalis",
        "builder: the record carries the description, as the game's records do");
    const Bytes &q = *built;
    const std::size_t rec = word(q, 0u);
    check(std::memcmp(q.data() + 4u, "v005", 4u) == 0 && rec == 0x94u && q[rec] == 4u && half(q, rec + 0x2Cu) == 75u &&
            half(q, rec + 0x2Eu) == 1u && half(q, rec + 0x3Cu) == 75u && half(q, rec + 0x3Eu) == 58u &&
            q.size() % 16u == 0u,
        "builder: a hunt for the target, with the large monsters as the main ones");
    const std::size_t languages = word(q, rec + 0x18u), set = word(q, languages);
    check(word(q, languages + 24u) == set &&
            std::string(reinterpret_cast<const char *>(q.data() + word(q, set + 4u))) == "Hunt the Fatalis" &&
            std::string(reinterpret_cast<const char *>(q.data() + word(q, set + 8u))) == "Reward Zero\nTime Over",
        "builder: the texts, the same in every language");
    const std::size_t large = word(q, 0x20u), loads = word(q, large + 8u), spawns = word(q, large + 12u);
    check(word(q, large) == 1u && half(q, loads) == 75u && half(q, loads + 4u) == 58u &&
            half(q, loads + 8u) == 0xFFFFu && word(q, spawns) == 75u && half(q, spawns + 6u) == 1u &&
            real(q, spawns + 0x10u) == 1086.0f && real(q, spawns + 0x18u) == 255.0f && word(q, spawns + 0x30u) == 58u &&
            half(q, spawns + 0x60u) == 0xFFFFu,
        "builder: the large monsters' group, the kinds to load, and where each starts, ended");
    check(word(q, 0x30u) == 0x1E000078u && (word(q, 0x34u) & 0xFFu) == 0x13u && word(q, 0x38u) == 0x10000064u,
        "builder: each monster's health level, size and attack level");
    const std::size_t waves = word(q, 0x24u);
    check(word(q, waves) == word(q, waves + 8u) && word(q, word(q, waves) + 4u) == 0x9999u &&
            q[word(q, 0x08u)] == 0xFFu && half(q, word(q, 0x1Cu)) == 0xFFFFu && half(q, word(q, 0x28u)) == 0xFFFFu &&
            q[word(q, 0x2Cu)] == 0xFFu,
        "builder: no rewards, gathering or small monsters, each table in the form the game reads as empty");
    QuestSpec none = spec;
    none.monsters.clear();
    QuestSpec many = spec;
    many.monsters.assign(6u, spec.monsters[0]);
    QuestSpec game = spec;
    game.id = 101u;
    QuestSpec wordy = spec;
    wordy.texts.description.assign(30000u, 'x');
    check(!build_quest(none, &why) && why.find("1 to 5") != std::string::npos && !build_quest(many) &&
            !build_quest(game, &why) && why.find("62000") != std::string::npos && !build_quest(wordy, &why) &&
            why.find("larger") != std::string::npos,
        "builder: no monsters, too many, a game quest's id and texts too long are refused");

    // Rewards: a group of lines, then each line's items (chance, count, item).
    QuestSpec paid = spec;
    paid.rewards = {{{252u, 2u, 100u}}, {{595u, 1u, 20u}, {247u, 2u, 80u}}};
    const std::optional<Bytes> rewarded = build_quest(paid, &why);
    const std::size_t table = rewarded ? word(*rewarded, 0x08u) : 0u;
    check(rewarded && word(*rewarded, table) == 0x200u && word(*rewarded, table + 4u) == 0u &&
            (*rewarded)[table + 8u] == 0xFFu && (*rewarded)[table + 16u] == 100u && (*rewarded)[table + 17u] == 2u &&
            half(*rewarded, table + 18u) == 252u && word(*rewarded, table + 20u) == 0xFFFFFFFFu &&
            (*rewarded)[table + 24u] == 20u && half(*rewarded, table + 26u) == 595u &&
            (*rewarded)[table + 28u] == 80u && half(*rewarded, table + 30u) == 247u &&
            word(*rewarded, table + 32u) == 0xFFFFFFFFu,
        "builder: the main reward's lines, each item's chance, count and id, each line ended");
    QuestSpec uneven = spec;
    uneven.rewards = {{{252u, 1u, 60u}}};
    QuestSpec nothing = spec;
    nothing.rewards = {{{0u, 1u, 100u}}};
    QuestSpec endless = spec;
    endless.rewards.assign(41u, {{252u, 1u, 100u}});
    check(!build_quest(uneven, &why) && why.find("100") != std::string::npos && !build_quest(nothing) &&
            !build_quest(endless, &why) && why.find("reward lines") != std::string::npos,
        "builder: a line whose chances do not add up to 100, an item 0 and too many lines are refused");
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 4 && std::string(argv[1]) == "--decrypt")
        return decrypt_real(argv[2], argv[3], argc > 4 ? argv[4] : nullptr);
    test_sha1();
    test_cipher();
    test_record();
    test_builder();
    std::printf("%s\n", failures == 0 ? "all quest file checks passed" : "quest file checks FAILED");
    return failures == 0 ? 0 : 1;
}
