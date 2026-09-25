#pragma once

// Translation files for the game's own text.
//
// A translation file is a UTF-8 text file, one language each, that overrides
// entries of the game's in-memory text block (docs/TEXT_TRANSLATION.md). It
// lives beside Yakumo, not inside the disc image: the game's own text stays
// where it is, and any entry the file does not name falls back to it (the
// English of a patched disc, or the Japanese of an original one).
//
// The format is small on purpose, so a file can be written or reviewed by
// hand:
//
//     # comments start with # or ;
//     language = pt-BR
//     name = Português (Brasil)
//     2:20 = Cancelar
//
// `TABLE:ENTRY` names a table of the game's text block and an entry in it
// (docs/DEBUG_MENU.md). In a value, `\n`, `\t`, `\\` and `\#` are understood;
// anything else after a backslash is left as it is.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mhp3rd::text {

// One language the loader found in a translations folder.
struct Language {
    std::string code;  // "pt-BR"
    std::string name;  // "Português (Brasil)", or the code when the file has no name
    std::filesystem::path file;
};

// The key of one entry: the table and the entry index, packed.
[[nodiscard]] constexpr std::uint64_t key(std::uint16_t table, std::uint32_t entry) noexcept {
    return (static_cast<std::uint64_t>(table) << 32u) | entry;
}
[[nodiscard]] constexpr std::uint16_t table_of(std::uint64_t key) noexcept {
    return static_cast<std::uint16_t>(key >> 32u);
}
[[nodiscard]] constexpr std::uint32_t entry_of(std::uint64_t key) noexcept {
    return static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
}

// The translations read from one file.
class Translations {
public:
    [[nodiscard]] bool empty() const noexcept { return order_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }
    [[nodiscard]] const std::string &code() const noexcept { return code_; }
    [[nodiscard]] const std::string &name() const noexcept { return name_; }

    // The translation for `table`:`entry`, or null when the file names none.
    [[nodiscard]] const std::string *find(std::uint16_t table, std::uint32_t entry) const;
    [[nodiscard]] const std::string *find(std::uint64_t key) const;

    // Adds one entry, replacing an earlier one with the same key.
    void add(std::uint16_t table, std::uint32_t entry, std::string text);
    void add(std::uint64_t key, std::string text);

    // Every entry, in the order it was added.
    [[nodiscard]] const std::vector<std::pair<std::uint64_t, std::string>> &entries() const noexcept {
        return order_;
    }

    // How many bytes the strings take in an arena, terminators included.
    [[nodiscard]] std::size_t arena_bytes() const noexcept;

    // Parses one file's text. `fallback_code` names the language when the file
    // has no `language` line (its own file name, in practice).
    [[nodiscard]] static Translations parse(const std::string &text, const std::string &fallback_code);

    // Reads one file. `error` is set, and nothing is returned, when the file
    // cannot be read.
    [[nodiscard]] static std::optional<Translations> from_file(const std::filesystem::path &file, std::string &error);

private:
    std::string code_;
    std::string name_;
    std::map<std::uint64_t, std::size_t> index_;  // key -> position in order_
    std::vector<std::pair<std::uint64_t, std::string>> order_;
};

// Every `*.lang` file in `directory`, sorted by language code. An unreadable
// or nameless file is skipped.
[[nodiscard]] std::vector<Language> scan_languages(const std::filesystem::path &directory);

} // namespace mhp3rd::text
