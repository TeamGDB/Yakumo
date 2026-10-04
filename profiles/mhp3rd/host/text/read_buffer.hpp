#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace mhp3rd::text {

// Archive reads can overlap or arrive out of order. Only the continuously
// received prefix is exposed to the parser; unread gaps are never fabricated.
class ReadBuffer {
public:
    static constexpr std::size_t kMaxBytes = 4u * 1024u * 1024u;
    static constexpr std::size_t kMaxFragments = 4096u;

    bool append(std::size_t total, std::size_t offset, std::span<const std::uint8_t> bytes) {
        if (bytes.empty() || total == 0 || total > kMaxBytes || offset > total || bytes.size() > total - offset)
            return false;
        if (data_.empty()) data_.resize(total);
        if (data_.size() != total) return false;
        std::size_t begin = offset, end = offset + bytes.size();
        auto it = ranges_.lower_bound(begin);
        if (it != ranges_.begin() && std::prev(it)->second >= begin) --it;
        auto first = it;
        while (it != ranges_.end() && it->first <= end) {
            begin = std::min(begin, it->first);
            end = std::max(end, it->second);
            ++it;
        }
        if (first == it && ranges_.size() >= kMaxFragments) return false;
        ranges_.erase(first, it);
        ranges_.emplace(begin, end);
        std::copy(bytes.begin(), bytes.end(), data_.begin() + offset);
        return true;
    }

    [[nodiscard]] std::span<const std::uint8_t> prefix() const noexcept {
        if (ranges_.empty() || ranges_.begin()->first != 0) return {};
        return std::span(data_).first(ranges_.begin()->second);
    }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
    void clear() {
        data_.clear();
        ranges_.clear();
    }

private:
    std::vector<std::uint8_t> data_;
    std::map<std::size_t, std::size_t> ranges_;
};

} // namespace mhp3rd::text
