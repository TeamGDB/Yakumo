#include "mods/mod_package.hpp"

#include "mods/mod_ini.hpp"

#include "psprecomp/deflate.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace mhp3rd::mods {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kLocalHeader = 0x04034B50u;
constexpr std::uint32_t kCentralHeader = 0x02014B50u;
constexpr std::uint32_t kEndOfDirectory = 0x06054B50u;
constexpr std::size_t kEndSize = 22u;
constexpr std::size_t kCentralSize = 46u;
constexpr std::size_t kLocalSize = 30u;
constexpr std::size_t kMaxComment = 0xFFFFu;
constexpr std::size_t kMaxName = 1024u;

std::uint16_t le16(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | bytes[at + 1u] << 8u);
}

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t at) {
    return static_cast<std::uint32_t>(le16(bytes, at)) | static_cast<std::uint32_t>(le16(bytes, at + 2u)) << 16u;
}

// CRC-32 as zip uses it (the reflected polynomial 0xEDB88320).
std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0u; i < 256u; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1u) : c >> 1u;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t byte : bytes) crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8u);
    return crc ^ 0xFFFFFFFFu;
}

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0u; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const std::size_t length = c < 0x80u ? 1u
            : (c >> 5u) == 0x6u              ? 2u
            : (c >> 4u) == 0xEu              ? 3u
            : (c >> 3u) == 0x1Eu             ? 4u
                                             : 0u;
        if (length == 0u || i + length > text.size()) return false;
        for (std::size_t k = 1u; k < length; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0u) != 0x80u) return false;
        i += length;
    }
    return true;
}

struct Entry {
    std::string name; // '/'-separated, checked
    bool folder{};
    std::uint16_t method{};
    std::uint32_t crc{};
    std::uint32_t packed{};
    std::uint32_t size{};
    std::uint32_t local{};
};

// A part Windows cannot hold as a file of its own: a device name (CON,
// NUL, COM1, ...) with or without an extension, or one ending in a dot or a
// space, which Windows drops, so that two names in the package would be one.
bool windows_reserved(std::string_view part) {
    if (part.back() == '.' || part.back() == ' ') return true;
    std::string stem(part.substr(0, part.find('.')));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return std::toupper(c); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") return true;
    return stem.size() == 4u && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' &&
        stem[3] <= '9';
}

// A name inside the package as a relative path that stays in the folder, or
// why not. Backslashes count as separators, as Windows tools write them.
std::optional<std::string> checked_name(std::string name, std::string &why) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.size() > kMaxName || !valid_utf8(name)) {
        why = "a name in it is empty, too long or not readable text";
        return std::nullopt;
    }
    if (name.front() == '/' || name.find(':') != std::string::npos) {
        why = "\"" + name + "\" points outside the package";
        return std::nullopt;
    }
    const bool folder = name.back() == '/';
    if (folder) name.pop_back();
    std::size_t start = 0u;
    while (start <= name.size()) {
        const std::size_t end = std::min(name.find('/', start), name.size());
        const std::string_view part(name.data() + start, end - start);
        if (part.empty() || part == "." || part == ".." ||
            std::any_of(part.begin(), part.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20u; })) {
            why = "\"" + name + "\" points outside the package";
            return std::nullopt;
        }
        if (windows_reserved(part)) {
            why = "\"" + name + "\" cannot be a file name on Windows";
            return std::nullopt;
        }
        start = end + 1u;
    }
    return folder ? name + "/" : name;
}

// Files a packer adds that are not part of the mod.
bool clutter(std::string_view name) {
    return name.starts_with("__MACOSX/") || name == ".DS_Store" || name.ends_with("/.DS_Store") ||
        name == "Thumbs.db" || name.ends_with("/Thumbs.db");
}

std::string amount(std::uint64_t bytes) {
    return bytes >= (1u << 20u) ? std::to_string(bytes >> 20u) + " MiB" : std::to_string(bytes) + " bytes";
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

class Reader {
public:
    explicit Reader(const fs::path &path) : in_(path, std::ios::binary) {
        if (in_) {
            in_.seekg(0, std::ios::end);
            size_ = static_cast<std::uint64_t>(in_.tellg());
        }
    }
    [[nodiscard]] bool open() const { return static_cast<bool>(in_); }
    [[nodiscard]] std::uint64_t size() const { return size_; }
    [[nodiscard]] bool read(std::uint64_t at, std::vector<std::uint8_t> &out, std::size_t length) {
        if (at > size_ || length > size_ - at) return false;
        out.resize(length);
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(at));
        return length == 0u ||
            static_cast<bool>(in_.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(length)));
    }

private:
    std::ifstream in_;
    std::uint64_t size_{};
};

// The entries the central directory lists, checked against the limits.
std::optional<std::vector<Entry>> directory(Reader &reader, const PackageLimits &limits, std::string &why) {
    const std::uint64_t size = reader.size();
    if (size < kEndSize) {
        why = "it is not a .ymod package (too short)";
        return std::nullopt;
    }
    std::vector<std::uint8_t> tail;
    const std::size_t span = static_cast<std::size_t>(std::min<std::uint64_t>(size, kEndSize + kMaxComment));
    if (!reader.read(size - span, tail, span)) {
        why = "it cannot be read";
        return std::nullopt;
    }
    std::optional<std::size_t> end;
    for (std::size_t at = tail.size() - kEndSize + 1u; at-- > 0u;) {
        if (le32(tail, at) == kEndOfDirectory && at + kEndSize + le16(tail, at + 20u) == tail.size()) {
            end = at;
            break;
        }
    }
    if (!end) {
        why = "it is not a .ymod package (no zip directory)";
        return std::nullopt;
    }
    const std::uint16_t disk = le16(tail, *end + 4u), directory_disk = le16(tail, *end + 6u);
    const std::uint16_t here = le16(tail, *end + 8u), count = le16(tail, *end + 10u);
    const std::uint32_t directory_size = le32(tail, *end + 12u), directory_offset = le32(tail, *end + 16u);
    const std::uint64_t end_offset = size - span + *end;
    if (disk != 0u || directory_disk != 0u || here != count || count == 0xFFFFu || directory_offset == 0xFFFFFFFFu ||
        static_cast<std::uint64_t>(directory_offset) + directory_size > end_offset) {
        why = "it is split over several files, uses Zip64 or is damaged";
        return std::nullopt;
    }
    if (count > limits.entries) {
        why = "it holds more than " + std::to_string(limits.entries) + " entries";
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes;
    if (!reader.read(directory_offset, bytes, directory_size)) {
        why = "its zip directory cannot be read";
        return std::nullopt;
    }

    std::vector<Entry> entries;
    std::set<std::string> names;
    std::uint64_t total = 0u;
    std::size_t at = 0u;
    for (std::uint16_t i = 0u; i < count; ++i) {
        if (at + kCentralSize > bytes.size() || le32(bytes, at) != kCentralHeader) {
            why = "its zip directory is damaged";
            return std::nullopt;
        }
        const std::uint16_t made_by = le16(bytes, at + 4u), flags = le16(bytes, at + 8u);
        const std::uint16_t name_length = le16(bytes, at + 28u), extra = le16(bytes, at + 30u),
                            comment = le16(bytes, at + 32u);
        if (at + kCentralSize + name_length + extra + comment > bytes.size()) {
            why = "its zip directory is damaged";
            return std::nullopt;
        }
        Entry entry;
        entry.method = le16(bytes, at + 10u);
        entry.crc = le32(bytes, at + 16u);
        entry.packed = le32(bytes, at + 20u);
        entry.size = le32(bytes, at + 24u);
        entry.local = le32(bytes, at + 42u);
        const std::uint32_t attributes = le32(bytes, at + 38u);
        std::string raw(reinterpret_cast<const char *>(bytes.data() + at + kCentralSize), name_length);
        at += kCentralSize + name_length + extra + comment;

        if ((flags & 0x41u) != 0u) {
            why = "it is encrypted";
            return std::nullopt;
        }
        if (entry.packed == 0xFFFFFFFFu || entry.size == 0xFFFFFFFFu || entry.local == 0xFFFFFFFFu) {
            why = "it uses Zip64, for files larger than Yakumo reads";
            return std::nullopt;
        }
        // Unix mode in the upper half: a symbolic link could point anywhere.
        if ((made_by >> 8u) == 3u && ((attributes >> 16u) & 0xF000u) == 0xA000u) {
            why = "it holds a link (\"" + raw + "\")";
            return std::nullopt;
        }
        const std::optional<std::string> name = checked_name(raw, why);
        if (!name) return std::nullopt;
        entry.folder = name->back() == '/';
        entry.name = entry.folder ? name->substr(0, name->size() - 1u) : *name;
        if (clutter(entry.name + (entry.folder ? "/" : ""))) continue;
        if (entry.folder) {
            entries.push_back(std::move(entry));
            continue;
        }
        if (entry.method != 0u && entry.method != 8u) {
            why = "\"" + entry.name + "\" is compressed with a method Yakumo does not read (" +
                std::to_string(entry.method) + "); pack it with plain zip compression";
            return std::nullopt;
        }
        if (entry.method == 0u && entry.packed != entry.size) {
            why = "its zip directory is damaged";
            return std::nullopt;
        }
        if (entry.size > limits.file_bytes || (total += entry.size) > limits.bytes) {
            why = "it unpacks to more than Yakumo accepts (" + amount(limits.file_bytes) + " a file, " +
                amount(limits.bytes) + " in all)";
            return std::nullopt;
        }
        // Two names for one file on a case-insensitive disk.
        if (!names.insert(lower(entry.name)).second) {
            why = "it holds \"" + entry.name + "\" twice";
            return std::nullopt;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

std::string unpack_entries(Reader &reader, const std::vector<Entry> &entries, const fs::path &folder) {
    std::vector<std::uint8_t> header, packed, data;
    std::error_code ec;
    for (const Entry &entry : entries) {
        const fs::path target = folder / from_utf8(entry.name);
        if (entry.folder) {
            fs::create_directories(target, ec);
            if (ec) return "cannot create " + entry.name + ": " + ec.message();
            continue;
        }
        if (!reader.read(entry.local, header, kLocalSize) || le32(header, 0u) != kLocalHeader)
            return "\"" + entry.name + "\" is damaged";
        const std::uint64_t start =
            static_cast<std::uint64_t>(entry.local) + kLocalSize + le16(header, 26u) + le16(header, 28u);
        if (!reader.read(start, packed, entry.packed)) return "\"" + entry.name + "\" is cut short";
        if (entry.method == 0u) {
            data.swap(packed);
        } else {
            const psprecomp::RawDeflateResult result = psprecomp::inflate_raw_deflate(packed, data, entry.size);
            if (result.status != psprecomp::RawDeflateStatus::Ok || data.size() != entry.size)
                return "\"" + entry.name + "\" is damaged";
        }
        if (crc32(data) != entry.crc) return "\"" + entry.name + "\" is damaged (its checksum does not match)";
        fs::create_directories(target.parent_path(), ec);
        if (ec) return "cannot create a folder for " + entry.name + ": " + ec.message();
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size())))
            return "cannot write " + entry.name;
    }
    return {};
}

} // namespace

bool is_package(const fs::path &path) {
    return lower(to_utf8(path.extension())) == ".ymod";
}

std::string unpack_package(const fs::path &package, const fs::path &folder, const PackageLimits &limits) {
    Reader reader(package);
    if (!reader.open()) return "it cannot be opened";
    std::error_code ec;
    if (fs::exists(folder, ec)) return "the folder to unpack it into exists already";
    std::string why;
    const std::optional<std::vector<Entry>> entries = directory(reader, limits, why);
    if (!entries) return why;
    fs::create_directories(folder, ec);
    if (ec) return "cannot create " + to_utf8(folder) + ": " + ec.message();
    why = unpack_entries(reader, *entries, folder);
    if (!why.empty()) fs::remove_all(folder, ec);
    return why;
}

} // namespace mhp3rd::mods
