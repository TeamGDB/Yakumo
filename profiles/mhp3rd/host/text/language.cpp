#include "text/language.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace mhp3rd::text {
namespace {

std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1u);
}

// `\n`, `\t`, `\\` and `\#` become themselves; any other escaped character is
// left with its backslash, so a literal one survives a round trip.
std::string unescape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1u >= text.size()) {
            out += text[i];
            continue;
        }
        const char next = text[i + 1u];
        switch (next) {
        case 'n': out += '\n'; ++i; break;
        case 't': out += '\t'; ++i; break;
        case '\\': out += '\\'; ++i; break;
        case '#': out += '#'; ++i; break;
        case ';': out += ';'; ++i; break;
        default: out += text[i]; break;
        }
    }
    return out;
}

// `TABLE:ENTRY` in decimal, as the file writes it.
bool parse_key(const std::string &text, std::uint16_t &table, std::uint32_t &entry) {
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0u || colon + 1u >= text.size()) return false;
    const std::string table_text = text.substr(0, colon);
    char *end = nullptr;
    const unsigned long t = std::strtoul(table_text.c_str(), &end, 10);
    if (end == table_text.c_str() || *end != '\0') return false;
    const std::string entry_text = text.substr(colon + 1u);
    const unsigned long e = std::strtoul(entry_text.c_str(), &end, 10);
    if (end == entry_text.c_str() || *end != '\0') return false;
    if (t > 0xFFFFul) return false;
    table = static_cast<std::uint16_t>(t);
    entry = static_cast<std::uint32_t>(e);
    return true;
}

} // namespace

const std::string *Translations::find(std::uint16_t table, std::uint32_t entry) const {
    return find(key(table, entry));
}

const std::string *Translations::find(std::uint64_t id) const {
    const auto found = index_.find(id);
    return found != index_.end() ? &order_[found->second].second : nullptr;
}

void Translations::add(std::uint16_t table, std::uint32_t entry, std::string text) {
    add(key(table, entry), std::move(text));
}

void Translations::add(std::uint64_t id, std::string text) {
    const auto found = index_.find(id);
    if (found != index_.end()) {
        order_[found->second].second = std::move(text);
        return;
    }
    index_.emplace(id, order_.size());
    order_.emplace_back(id, std::move(text));
}

std::size_t Translations::arena_bytes() const noexcept {
    std::size_t total = 0u;
    for (const auto &[id, text] : order_) total += text.size() + 1u;
    return total;
}

Translations Translations::parse(const std::string &text, const std::string &fallback_code) {
    Translations translations;
    translations.code_ = fallback_code;
    translations.name_ = fallback_code;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        // Only the line ending is trimmed away: a value may end in the spaces
        // the game's own text uses for layout.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#' || line[start] == ';') continue;
        const auto equals = line.find('=', start);
        if (equals == std::string::npos) continue;
        const std::string name = trimmed(line.substr(start, equals - start));
        std::string value = line.substr(equals + 1u);
        // One separating space is the convention; the rest of the value is
        // kept as written.
        if (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0u, 1u);
        value = unescape(value);
        if (name == "language") {
            if (!value.empty()) translations.code_ = value;
            continue;
        }
        if (name == "name") {
            if (!value.empty()) translations.name_ = value;
            continue;
        }
        std::uint16_t table = 0u;
        std::uint32_t entry = 0u;
        if (!parse_key(name, table, entry)) continue;
        translations.add(table, entry, std::move(value));
    }
    return translations;
}

std::optional<Translations> Translations::from_file(const std::filesystem::path &file, std::string &error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open " + file.string();
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) {
        error = "cannot read " + file.string();
        return std::nullopt;
    }
    return parse(buffer.str(), file.stem().string());
}

std::vector<Language> scan_languages(const std::filesystem::path &directory) {
    std::vector<Language> languages;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return languages;
    for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".lang") continue;
        std::string error;
        const std::optional<Translations> translations = Translations::from_file(entry.path(), error);
        if (!translations || translations->code().empty()) continue;
        languages.push_back(Language{translations->code(), translations->name(), entry.path()});
    }
    std::sort(languages.begin(), languages.end(),
              [](const Language &a, const Language &b) { return a.code < b.code; });
    return languages;
}

} // namespace mhp3rd::text
