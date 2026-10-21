#include "game/pack_file.hpp"

#include <cstring>

namespace mhp3rd::game {
namespace {

std::uint32_t u32(std::span<const std::uint8_t> data, std::size_t at) {
    std::uint32_t v = 0u;
    if (at + 4u <= data.size()) std::memcpy(&v, data.data() + at, 4u);
    return v;
}

void put32(std::vector<std::uint8_t> &data, std::size_t at, std::uint32_t v) {
    std::memcpy(data.data() + at, &v, 4u);
}

constexpr std::uint32_t kMostEntries = 64u;

} // namespace

std::optional<std::vector<std::span<const std::uint8_t>>> read_pack(std::span<const std::uint8_t> data) {
    const std::uint32_t count = u32(data, 0u);
    if (data.size() < 4u || count > kMostEntries || 4u + 8u * std::size_t{count} > data.size()) return std::nullopt;
    std::vector<std::span<const std::uint8_t>> entries;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t offset = u32(data, 4u + 8u * i), size = u32(data, 8u + 8u * i);
        if (size != 0u && (offset > data.size() || size > data.size() - offset)) return std::nullopt;
        entries.push_back(size != 0u ? data.subspan(offset, size) : std::span<const std::uint8_t>{});
    }
    return entries;
}

std::vector<std::uint8_t> write_pack(const std::vector<std::vector<std::uint8_t>> &entries) {
    const auto aligned = [](std::size_t n) { return (n + 15u) & ~std::size_t{15u}; };
    std::vector<std::uint8_t> out(aligned(4u + 8u * entries.size()), 0u);
    put32(out, 0u, static_cast<std::uint32_t>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].empty()) continue;
        put32(out, 4u + 8u * i, static_cast<std::uint32_t>(out.size()));
        put32(out, 8u + 8u * i, static_cast<std::uint32_t>(entries[i].size()));
        out.insert(out.end(), entries[i].begin(), entries[i].end());
        out.resize(aligned(out.size()), 0u);
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> replace_entries(std::span<const std::uint8_t> pack,
    const std::vector<std::pair<std::size_t, std::span<const std::uint8_t>>> &replacements) {
    const auto entries = read_pack(pack);
    if (!entries) return std::nullopt;
    std::vector<std::vector<std::uint8_t>> out;
    for (const std::span<const std::uint8_t> e : *entries) out.emplace_back(e.begin(), e.end());
    for (const auto &[index, bytes] : replacements) {
        if (index >= out.size()) return std::nullopt;
        out[index].assign(bytes.begin(), bytes.end());
    }
    return write_pack(out);
}

} // namespace mhp3rd::game
