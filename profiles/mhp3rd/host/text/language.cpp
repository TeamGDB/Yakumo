#include "text/language.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <limits>
#include <fstream>
#include <sstream>
#include <system_error>

namespace mhp3rd::text {
namespace {

std::string path_text(const std::filesystem::path &path) {
    const auto bytes = path.u8string();
    return std::string(bytes.begin(), bytes.end());
}

std::string trimmed(const std::string &text) {
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
        switch (text[i + 1u]) {
        case 'n':
            out += '\n';
            ++i;
            break;
        case 'r':
            out += '\r';
            ++i;
            break;
        case 't':
            out += '\t';
            ++i;
            break;
        case '\\':
            out += '\\';
            ++i;
            break;
        case '#':
            out += '#';
            ++i;
            break;
        case ';':
            out += ';';
            ++i;
            break;
        default:
            out += text[i];
            break;
        }
    }
    return out;
}

// One non-negative decimal number in full.
bool number(const std::string &text, std::uint32_t &value) {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

// A key: `TABLE:INDEX`, `TABLE:FIRST-LAST` or with `*` for either side.
bool parse_key(const std::string &text, Pattern &pattern) {
    const auto colon = text.find(':');
    if (colon == std::string::npos || colon == 0u || colon + 1u >= text.size()) return false;
    const std::string table_text = trimmed(text.substr(0, colon));
    const std::string index_text = trimmed(text.substr(colon + 1u));

    if (table_text == "*") {
        pattern.any = Pattern::Any::Table;
    } else {
        std::uint32_t table = 0;
        if (!number(table_text, table) || table > 0xFFFFul) return false;
        pattern.table = static_cast<std::uint16_t>(table);
    }

    if (index_text == "*") {
        pattern.any = pattern.any == Pattern::Any::Table ? Pattern::Any::Both : Pattern::Any::Index;
        return true;
    }
    const auto dash = index_text.find('-');
    if (dash != std::string::npos && dash > 0u && dash + 1u < index_text.size()) {
        std::uint32_t first = 0, last = 0;
        if (!number(trimmed(index_text.substr(0, dash)), first) ||
            !number(trimmed(index_text.substr(dash + 1u)), last) || first > last)
            return false;
        pattern.first = static_cast<std::uint32_t>(first);
        pattern.last = static_cast<std::uint32_t>(last);
        pattern.has_range = true;
        return true;
    }
    std::uint32_t index = 0;
    if (!number(index_text, index)) return false;
    pattern.first = static_cast<std::uint32_t>(index);
    return true;
}

} // namespace

const std::string *Translations::find(std::uint16_t table, std::uint32_t index) const {
    return find(key(table, index));
}

const std::string *Translations::find(std::uint64_t id) const {
    const auto found = index_.find(id);
    if (found != index_.end()) return &order_[found->second].second;
    const std::uint16_t table = table_of(id);
    const std::uint32_t index = index_of(id);
    for (const auto &[pattern, text] : patterns_)
        if (pattern.matches(table, index)) return &text;
    return nullptr;
}

void Translations::add(std::uint16_t table, std::uint32_t index, std::string text) {
    add(key(table, index), std::move(text));
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

void Translations::add(Pattern pattern, std::string text) {
    patterns_.emplace_back(std::move(pattern), std::move(text));
}

std::size_t Translations::arena_bytes() const noexcept {
    return arena_bytes({});
}

std::size_t Translations::arena_bytes(const std::vector<std::uint32_t> &table_sizes) const noexcept {
    std::size_t total = 0u;
    for (const auto &[id, text] : order_) total += text.size() + 1u;
    for (const auto &[pattern, text] : patterns_) {
        if (table_sizes.empty()) {
            total += text.size() + 1u;
            continue;
        }
        std::size_t matches = 0u;
        for (std::uint16_t table = 0; table < table_sizes.size(); ++table) {
            if (pattern.any != Pattern::Any::Table && pattern.any != Pattern::Any::Index &&
                pattern.any != Pattern::Any::Both && table != pattern.table)
                continue;
            const std::uint32_t count = table_sizes[table];
            for (std::uint32_t index = 1u; index < count; ++index)
                if (pattern.matches(table, index)) ++matches;
        }
        total += matches * (text.size() + 1u);
    }
    return total;
}

Translations Translations::parse(const std::string &text, const std::string &fallback_code) {
    // One block's worth: the main block of whatever the text holds.
    return parse_blocks(text, fallback_code).at(kMainEntry);
}

std::map<std::uint32_t, Translations> Translations::parse_blocks(
    const std::string &text, const std::string &fallback_code) {
    std::map<std::uint32_t, Translations> blocks;
    std::string code = fallback_code;
    std::string name = fallback_code;
    std::uint32_t current = kMainEntry;
    bool any_header = false;

    std::istringstream stream(text.starts_with("\xef\xbb\xbf") ? text.substr(3) : text);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#' || line[start] == ';') continue;
        const std::string body = line.substr(start);

        // A section header: [entry], [main] or [entry:name].
        if (body.front() == '[') {
            const auto close = body.find(']');
            if (close == std::string::npos) continue;
            std::string head = trimmed(body.substr(1u, close - 1u));
            const auto colon = head.find(':');
            if (colon != std::string::npos) head = head.substr(0, colon);
            if (head == "main")
                current = kMainEntry;
            else {
                std::uint32_t entry = 0;
                if (!number(head, entry)) continue;
                current = static_cast<std::uint32_t>(entry);
            }
            any_header = true;
            blocks.try_emplace(current);
            continue;
        }

        const auto equals = body.find('=');
        if (equals == std::string::npos) continue;
        const std::string key_text = trimmed(body.substr(0, equals));
        std::string value = body.substr(equals + 1u);
        if (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0u, 1u);
        value = unescape(value);

        if (key_text == "language") {
            code = value.empty() ? fallback_code : value;
            continue;
        }
        if (key_text == "name") {
            name = value.empty() ? fallback_code : value;
            continue;
        }

        Pattern pattern;
        if (!parse_key(key_text, pattern)) continue;
        // A plain `table:index` is an exact key; a range or a `*` is a rule.
        if (pattern.any == Pattern::Any::None && !pattern.has_range)
            blocks[current].add(pattern.table, pattern.first, std::move(value));
        else
            blocks[current].add(std::move(pattern), std::move(value));
    }

    // A file the loader recognised but that named no block still has the main
    // one, so a caller can find the language's name.
    if (blocks.empty()) blocks.try_emplace(kMainEntry);
    for (auto &[entry, translations] : blocks) {
        if (translations.code().empty() || !any_header) translations.set_code(code);
        translations.set_code(code);
        translations.set_name(name);
    }
    return blocks;
}

namespace {
bool valid_utf8(const std::string &text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first == 0) return false;
        if (first < 0x80) continue;
        unsigned trailing = 0;
        std::uint32_t value = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            trailing = 1;
            value = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            trailing = 2;
            value = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            trailing = 3;
            value = first & 7;
            minimum = 0x10000;
        } else
            return false;
        if (trailing > text.size() - i) return false;
        for (unsigned n = 0; n < trailing; ++n) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
bool valid_code(const std::string &code) {
    if (code.empty() || code.size() > 64) return false;
    return std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
}

std::optional<std::map<std::uint32_t, Translations>> Translations::from_file(
    const std::filesystem::path &file, std::string &error, std::string *validated_contents) {
    error.clear();
    if (validated_contents) validated_contents->clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        error = "not a readable regular translation file";
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot open translation file";
        return std::nullopt;
    }
    std::string contents;
    std::array<char, 65536> chunk{};
    std::size_t line_bytes = 0;
    while (in) {
        in.read(chunk.data(), chunk.size());
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got > kMaxTranslationBytes - contents.size()) {
            error = "translation file exceeds 16 MiB";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < got; ++i) {
            if (chunk[i] == '\n')
                line_bytes = 0;
            else if (++line_bytes > kMaxTranslationLine) {
                error = "translation line exceeds 64 KiB";
                return std::nullopt;
            }
        }
        contents.append(chunk.data(), got);
    }
    if (!in.eof()) {
        error = "cannot read translation file";
        return std::nullopt;
    }
    if (!valid_utf8(contents)) {
        error = "translation must be UTF-8 without NUL bytes";
        return std::nullopt;
    }
    auto blocks = parse_blocks(contents, path_text(file.stem()));
    if (!valid_code(blocks.begin()->second.code())) {
        error = "language code must contain 1-64 ASCII letters, digits, hyphens or underscores";
        return std::nullopt;
    }
    if (validated_contents) *validated_contents = std::move(contents);
    return blocks;
}

std::vector<Language> scan_languages(const std::filesystem::path &directory) {
    std::vector<Language> languages;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return languages;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const auto &entry = *it;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".lang") continue;
        std::string error;
        const auto blocks = Translations::from_file(entry.path(), error);
        if (!blocks || blocks->empty()) continue;
        const Translations &main = blocks->begin()->second;
        Language language;
        language.code = main.code();
        language.name = main.name();
        language.file = entry.path();
        if (language.code.empty() || language.code == "original") continue;
        languages.push_back(std::move(language));
    }
    std::sort(languages.begin(), languages.end(), [](const Language &a, const Language &b) { return a.code < b.code; });
    return languages;
}

namespace {

// A language code cut down to what is safe in a file name: letters, digits,
// '-', '_' and '.'. Anything else becomes '_'.
std::string file_safe(const std::string &code) {
    std::string out;
    out.reserve(code.size());
    for (const char c : code) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.';
        out += ok ? c : '_';
    }
    return out.empty() ? std::string("translation") : out;
}

} // namespace

TranslationImport import_translation_file(const std::filesystem::path &source, const std::filesystem::path &folder) {
    TranslationImport result;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        result.error = "not a file: " + path_text(source);
        return result;
    }
    // Read it first: a file that is not a translation is refused before anything
    // is written into the loader's folder.
    std::string error;
    std::string contents;
    const auto blocks = Translations::from_file(source, error, &contents);
    if (!blocks || blocks->empty()) {
        result.error = error.empty() ? "not a translation file" : error;
        return result;
    }
    // A file with no string at all (a text file that is not a translation) is
    // refused, so importing junk does not leave a dead entry in the choice.
    bool any = false;
    for (const auto &[entry, translations] : *blocks)
        if (!translations.empty()) {
            any = true;
            break;
        }
    if (!any) {
        result.error = "no strings in it";
        return result;
    }
    const Translations &main = blocks->begin()->second;
    result.code = main.code();
    result.name = main.name();
    if (result.code == "original") {
        result.error = "original is reserved for the unmodified game text";
        return result;
    }
    if (result.code.empty()) result.code = path_text(source.stem());
    if (result.code.empty()) result.code = "translation";

    std::filesystem::create_directories(folder, ec);
    if (ec) {
        result.error = "cannot make " + path_text(folder) + ": " + ec.message();
        return result;
    }
    const std::filesystem::path destination = folder / (file_safe(result.code) + ".lang");
    const auto status = std::filesystem::symlink_status(destination, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        result.error = "cannot inspect translation destination";
        return result;
    }
    ec.clear();
    const bool replacing = std::filesystem::exists(status);
    if (replacing && !std::filesystem::is_regular_file(status)) {
        result.error = "translation destination must be a regular file, not a symlink or directory";
        return result;
    }
    // An exclusively created staging directory holds the validated bytes and,
    // during replacement, the old file. Failed writes never truncate the old
    // translation. Source changes after validation cannot change the import.
    std::filesystem::path staging;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto candidate = folder / (".translation-import-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate, ec)) {
            staging = std::move(candidate);
            break;
        }
        if (ec) break;
    }
    if (staging.empty()) {
        result.error = "cannot create translation staging directory";
        return result;
    }
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{staging};
    const auto staged = staging / "translation.lang";
    {
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.close();
        if (!out) {
            result.error = "cannot write staged translation";
            return result;
        }
    }
    const auto previous = staging / "previous.lang";
    if (replacing) {
        std::filesystem::rename(destination, previous, ec);
        if (ec) {
            result.error = "cannot preserve previous translation";
            return result;
        }
    }
    std::filesystem::rename(staged, destination, ec);
    if (ec) {
        result.error = "cannot install translation";
        if (replacing) {
            std::filesystem::rename(previous, destination, ec);
            if (ec) {
                // Keep the backup available for recovery when rollback fails.
                cleanup.path.clear();
                result.error += "; previous file remains in " + path_text(previous);
            }
        }
        return result;
    }
    result.saved = destination;
    return result;
}

} // namespace mhp3rd::text
