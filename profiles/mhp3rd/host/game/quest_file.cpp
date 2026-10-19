#include "game/quest_file.hpp"

#include "game/guest_ram.hpp"

#include <algorithm>
#include <cstring>

namespace mhp3rd::game {
namespace {

std::uint16_t load16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (bytes[at + 1u] << 8u));
}

std::uint32_t load32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1u]) << 8u |
        static_cast<std::uint32_t>(bytes[at + 2u]) << 16u | static_cast<std::uint32_t>(bytes[at + 3u]) << 24u;
}

void store16(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint16_t value) {
    bytes[at] = static_cast<std::uint8_t>(value);
    bytes[at + 1u] = static_cast<std::uint8_t>(value >> 8u);
}

// The four keys, stepped as the game steps them.
class KeyStream {
public:
    KeyStream(std::span<const std::uint8_t> seeds, const QuestKeys &keys) : keys_(keys) {
        for (std::size_t i = 0; i < 4u; ++i) {
            const std::uint16_t seed = load16(seeds, i * 2u);
            state_[i] = seed != 0u ? seed : static_cast<std::uint16_t>(keys.multipliers[i]);
        }
    }
    // XORs the 16-bit words of `bytes`, which start at byte `offset` of the file.
    void apply(std::vector<std::uint8_t> &bytes, std::size_t offset) {
        for (std::size_t at = 0; at + 1u < bytes.size(); at += 2u) {
            const std::size_t i = ((at + offset) >> 1u) & 3u;
            state_[i] = static_cast<std::uint16_t>(
                (static_cast<std::uint64_t>(state_[i]) * keys_.multipliers[i]) % keys_.moduli[i]);
            store16(bytes, at, static_cast<std::uint16_t>(load16(bytes, at) ^ state_[i]));
        }
    }

private:
    const QuestKeys &keys_;
    std::array<std::uint16_t, 4> state_{};
};

std::array<std::uint8_t, 20> digest(std::span<const std::uint8_t> payload, const QuestKeys &keys) {
    std::vector<std::uint8_t> salted(payload.begin(), payload.end());
    salted.insert(salted.end(), keys.salt.begin(), keys.salt.end());
    return sha1(salted);
}

std::uint32_t rotl(std::uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32u - bits));
}

} // namespace

std::optional<QuestKeys> read_quest_keys(const Ram &ram) {
    if (!ram.contains(kQuestMultipliers, 16u) || !ram.contains(kQuestModuli, 16u) || !ram.contains(kQuestSalt, 16u))
        return std::nullopt;
    QuestKeys keys;
    for (std::uint32_t i = 0; i < 4u; ++i) {
        keys.multipliers[i] = ram.load32(kQuestMultipliers + i * 4u);
        keys.moduli[i] = ram.load32(kQuestModuli + i * 4u);
        if (keys.multipliers[i] == 0u || keys.moduli[i] == 0u) return std::nullopt;
    }
    for (std::uint32_t i = 0; i < 16u; ++i) keys.salt[i] = ram.load8(kQuestSalt + i);
    return keys;
}

// FIPS 180-4.
std::array<std::uint8_t, 20> sha1(std::span<const std::uint8_t> bytes) {
    std::array<std::uint32_t, 5> h{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<std::uint8_t> message(bytes.begin(), bytes.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8u;
    message.push_back(0x80u);
    while (message.size() % 64u != 56u) message.push_back(0u);
    for (int shift = 56; shift >= 0; shift -= 8) message.push_back(static_cast<std::uint8_t>(bits >> shift));
    for (std::size_t block = 0; block < message.size(); block += 64u) {
        std::array<std::uint32_t, 80> w{};
        for (std::size_t i = 0; i < 16u; ++i) {
            const std::size_t at = block + i * 4u;
            w[i] = static_cast<std::uint32_t>(message[at]) << 24u | static_cast<std::uint32_t>(message[at + 1u]) << 16u |
                static_cast<std::uint32_t>(message[at + 2u]) << 8u | message[at + 3u];
        }
        for (std::size_t i = 16u; i < 80u; ++i) w[i] = rotl(w[i - 3u] ^ w[i - 8u] ^ w[i - 14u] ^ w[i - 16u], 1u);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (std::size_t i = 0; i < 80u; ++i) {
            std::uint32_t f = 0u, k = 0u;
            if (i < 20u) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40u) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60u) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t next = rotl(a, 5u) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30u);
            b = a;
            a = next;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 20u; ++i) out[i] = static_cast<std::uint8_t>(h[i / 4u] >> (24u - 8u * (i % 4u)));
    return out;
}

std::optional<std::vector<std::uint8_t>> decrypt_quest(
    std::span<const std::uint8_t> file, const QuestKeys &keys, std::string *why) {
    const auto refuse = [why](const char *reason) -> std::optional<std::vector<std::uint8_t>> {
        if (why != nullptr) *why = reason;
        return std::nullopt;
    };
    if (file.size() < kQuestHeader) return refuse("shorter than a quest file's header");
    KeyStream stream(file.first(8u), keys);
    std::vector<std::uint8_t> head(file.begin() + 8, file.begin() + kQuestHeader);
    stream.apply(head, 8u);
    const std::uint32_t size = load32(head, 0u);
    if (size == 0u || size > kQuestMaxPayload || size > file.size() - kQuestHeader)
        return refuse("its size is out of range");
    std::vector<std::uint8_t> payload(file.begin() + kQuestHeader, file.begin() + kQuestHeader + size);
    stream.apply(payload, kQuestHeader);
    const std::array<std::uint8_t, 20> expected = digest(payload, keys);
    if (!std::equal(expected.begin(), expected.end(), head.begin() + 4)) return refuse("its digest does not match");
    return payload;
}

std::vector<std::uint8_t> encrypt_quest(
    std::span<const std::uint8_t> payload, const QuestKeys &keys, std::array<std::uint16_t, 4> seeds) {
    std::vector<std::uint8_t> file(kQuestHeader, 0u);
    for (std::size_t i = 0; i < 4u; ++i) store16(file, i * 2u, seeds[i]);
    // The game decrypts whole 16-bit words: an odd payload gets a zero byte.
    std::vector<std::uint8_t> body(payload.begin(), payload.end());
    if (body.size() % 2u != 0u) body.push_back(0u);
    std::vector<std::uint8_t> head(kQuestHeader - 8u, 0u);
    const auto size = static_cast<std::uint32_t>(body.size());
    for (std::size_t i = 0; i < 4u; ++i) head[i] = static_cast<std::uint8_t>(size >> (8u * i));
    const std::array<std::uint8_t, 20> sum = digest(body, keys);
    std::copy(sum.begin(), sum.end(), head.begin() + 4);
    KeyStream stream(file, keys);
    stream.apply(head, 8u);
    stream.apply(body, kQuestHeader);
    std::copy(head.begin(), head.end(), file.begin() + 8);
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

std::optional<QuestRecord> read_quest_record(std::span<const std::uint8_t> payload) {
    if (payload.size() < 4u) return std::nullopt;
    const std::uint32_t offset = load32(payload, 0u);
    if (offset < 4u || offset > payload.size() || payload.size() - offset < 0x48u) return std::nullopt;
    QuestRecord record;
    record.offset = offset;
    record.fee = load32(payload, offset + 4u);
    record.reward = load32(payload, offset + 8u);
    record.frames = load32(payload, offset + 0x10u);
    record.id = load16(payload, offset + 0x1Cu);
    record.stars = load16(payload, offset + 0x1Eu);
    record.map = payload[offset + 0x20u];
    const std::size_t name = offset + 0x48u;
    const auto end = std::find(payload.begin() + static_cast<std::ptrdiff_t>(name), payload.end(), std::uint8_t{0});
    record.name.assign(payload.begin() + static_cast<std::ptrdiff_t>(name), end);
    return record;
}

} // namespace mhp3rd::game
