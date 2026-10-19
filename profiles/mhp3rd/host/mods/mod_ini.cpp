#include "mods/mod_ini.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>

namespace mhp3rd::mods {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1u));
}

std::string to_utf8(const std::filesystem::path &path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path from_utf8(std::string_view text) {
    return std::u8string(text.begin(), text.end());
}

std::vector<std::string> split_list(std::string_view value, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0u;
    while (start <= value.size()) {
        const auto end = value.find(separator, start);
        std::string part =
            trim(value.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (!part.empty()) parts.push_back(std::move(part));
        if (end == std::string_view::npos) break;
        start = end + 1u;
    }
    return parts;
}

namespace {

// `"text"` -> text; `"text` -> text; text -> text.
std::string unquote(std::string_view raw) {
    std::string value = trim(raw);
    if (value.empty() || value.front() != '"') return value;
    const auto close = value.find('"', 1u);
    if (close == std::string::npos) return trim(std::string_view(value).substr(1u));
    return value.substr(1u, close - 1u);
}

} // namespace

IniFile IniFile::parse(std::string_view text) {
    IniFile file;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3u);
    Section *current = nullptr;
    const auto open_section = [&file](std::string name) -> Section * {
        for (Section &existing : file.sections_) {
            if (iequals(existing.name, name)) return &existing;
        }
        file.sections_.push_back(Section{std::move(name), {}});
        return &file.sections_.back();
    };
    std::size_t start = 0u;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string line = trim(text.substr(start, end - start));
        start = end + 1u;
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            current = open_section(
                trim(std::string_view(line).substr(1u, close == std::string::npos ? std::string::npos : close - 1u)));
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        if (current == nullptr) current = open_section({});
        current->entries.emplace_back(
            trim(std::string_view(line).substr(0u, equals)), unquote(std::string_view(line).substr(equals + 1u)));
    }
    return file;
}

std::optional<std::vector<std::uint8_t>> parse_bytes(std::string_view text, std::string &error) {
    std::vector<std::uint8_t> bytes;
    std::size_t at = 0u;
    while (at < text.size()) {
        if (std::isspace(static_cast<unsigned char>(text[at])) != 0) {
            ++at;
            continue;
        }
        std::size_t end = at;
        while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) ++end;
        std::string_view token = text.substr(at, end - at);
        at = end;
        const bool number = token.size() > 2u && token[0] == '0' && (token[1] == 'x' || token[1] == 'X');
        if (number) token.remove_prefix(2);
        if (!std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }) ||
            token.size() % 2u != 0u || (number && token.size() != 2u && token.size() != 4u && token.size() != 8u)) {
            error = "\"" + std::string(number ? "0x" : "") + std::string(token) +
                "\" is not bytes: write pairs of hex digits, or 0x and 2, 4 or 8 hex digits";
            return std::nullopt;
        }
        const auto digit = [](char c) {
            return static_cast<std::uint8_t>(std::isdigit(static_cast<unsigned char>(c)) != 0
                    ? c - '0'
                    : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
        };
        std::vector<std::uint8_t> parsed;
        for (std::size_t i = 0; i < token.size(); i += 2u)
            parsed.push_back(static_cast<std::uint8_t>(digit(token[i]) << 4u | digit(token[i + 1u])));
        if (number) std::reverse(parsed.begin(), parsed.end()); // a number is stored low byte first
        bytes.insert(bytes.end(), parsed.begin(), parsed.end());
    }
    if (bytes.empty()) {
        error = "no bytes given";
        return std::nullopt;
    }
    return bytes;
}

std::optional<Edit> parse_edit(std::string_view key, std::string_view value, std::string &error) {
    if (const auto comment = value.find_first_of(";#"); comment != std::string_view::npos)
        value = value.substr(0, comment);
    std::string address = trim(key);
    if (address.size() > 2u && address[0] == '0' && (address[1] == 'x' || address[1] == 'X')) address.erase(0, 2);
    Edit edit;
    const auto [end, ec] = std::from_chars(address.data(), address.data() + address.size(), edit.address, 16);
    if (address.empty() || address.size() > 8u || ec != std::errc{} || end != address.data() + address.size()) {
        error = "\"" + trim(key) + "\" is not an address: write it in hex";
        return std::nullopt;
    }
    std::string_view written = value;
    if (const auto arrow = value.find("->"); arrow != std::string_view::npos) {
        std::optional<std::vector<std::uint8_t>> expect = parse_bytes(value.substr(0, arrow), error);
        if (!expect) return std::nullopt;
        edit.expect = std::move(*expect);
        written = value.substr(arrow + 2u);
    }
    std::optional<std::vector<std::uint8_t>> bytes = parse_bytes(written, error);
    if (!bytes) return std::nullopt;
    edit.bytes = std::move(*bytes);
    if (!edit.expect.empty() && edit.expect.size() != edit.bytes.size()) {
        error = "it expects " + std::to_string(edit.expect.size()) + " bytes but writes " +
            std::to_string(edit.bytes.size());
        return std::nullopt;
    }
    return edit;
}

std::optional<IniFile> IniFile::load(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return parse(text);
}

const IniFile::Section *IniFile::section(std::string_view name) const {
    for (const Section &s : sections_) {
        if (iequals(s.name, name)) return &s;
    }
    return nullptr;
}

bool IniFile::has_section(std::string_view name) const {
    return section(name) != nullptr;
}

const std::string *IniFile::find(std::string_view section_name, std::string_view key) const {
    const Section *s = section(section_name);
    if (s == nullptr) return nullptr;
    for (auto it = s->entries.rbegin(); it != s->entries.rend(); ++it) {
        if (iequals(it->first, key)) return &it->second;
    }
    return nullptr;
}

std::string IniFile::get(std::string_view section_name, std::string_view key, std::string_view fallback) const {
    const std::string *value = find(section_name, key);
    return value != nullptr ? *value : std::string(fallback);
}

std::vector<std::string> IniFile::sections() const {
    std::vector<std::string> names;
    names.reserve(sections_.size());
    for (const Section &s : sections_) names.push_back(s.name);
    return names;
}

const IniFile::Entries &IniFile::entries(std::string_view name) const {
    static const Entries kEmpty;
    const Section *s = section(name);
    return s != nullptr ? s->entries : kEmpty;
}

} // namespace mhp3rd::mods
