#pragma once

// Translation files for the game's own text.
//
// A translation file is a UTF-8 text file, one language each, that overrides
// strings of the game's text blocks (docs/TEXT_TRANSLATION.md). It lives beside
// Yakumo, not inside the disc image: the game's own text stays where it is, and
// any string the file does not name falls back to it (the English of a patched
// disc, or the Japanese of an original one).
//
// The format groups the strings by the archive file they come from, because the
// game keeps several text blocks (its main one, a quest's, a menu's) and the
// same `table:index` means different things in each:
//
//     # comments start with # or ;
//     language = pt-BR
//     name = Português (Brasil)
//
//     [16]                   # the main block; `[main]` is a shorthand for it
//     2:20 = Cancelar        # table 2, string 20
//     3:8  = Poção
//
//     [2835]
//     2:129 = Bem-vindo ao mundo de Monster Hunter.
//
// A file written in the old shape (no `[entry]`, `table:index = text` lines at
// the top level) is read as the main block, so the older files still load.
//
// A key may name a range or a wildcard instead of one string, which keeps a
// file small when the same text repeats:
//
//     [16]
//     2:308-382 = ...        # every string of table 2 from 308 through 382
//     2:* = ...              # every string of table 2
//     *:5  = ...             # string 5 of every table of the block
//
// In a value, `\n`, `\t`, `\\` and `\#` are understood; anything else after a
// backslash is left as it is.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mhp3rd::text {

// The archive entry of the big shared block the game loads at start
// (docs/DEBUG_MENU.md). `[main]` in a file means this.
inline constexpr std::uint32_t kMainEntry = 16u;
inline constexpr std::size_t kMaxTranslationBytes = 16u * 1024u * 1024u;
inline constexpr std::size_t kMaxTranslationLine = 64u * 1024u;

// One language the loader found in a translations folder.
struct Language {
    std::string code; // "pt-BR"
    std::string name; // "Português (Brasil)", or the code when the file has no name
    std::filesystem::path file;
};

// The key of one string: the table and the index in it, packed. The block the
// string belongs to is kept apart (one Translations holds one block).
[[nodiscard]] constexpr std::uint64_t key(std::uint16_t table, std::uint32_t index) noexcept {
    return (static_cast<std::uint64_t>(table) << 32u) | index;
}
[[nodiscard]] constexpr std::uint16_t table_of(std::uint64_t key) noexcept {
    return static_cast<std::uint16_t>(key >> 32u);
}
[[nodiscard]] constexpr std::uint32_t index_of(std::uint64_t key) noexcept {
    return static_cast<std::uint32_t>(key & 0xFFFFFFFFu);
}

// A table wildcard: which table and index a key names. `*` is a wildcard.
struct Pattern {
    enum class Any { None, Table, Index, Both } any{Any::None};
    bool has_range{};
    std::uint16_t table{};
    std::uint32_t first{};
    std::uint32_t last{};

    [[nodiscard]] bool matches(std::uint16_t candidate_table, std::uint32_t candidate_index) const noexcept {
        if (any != Any::Table && any != Any::Both && candidate_table != table) return false;
        if (any == Any::Index || any == Any::Both) return true; // table:* matches every index
        return has_range ? candidate_index >= first && candidate_index <= last : candidate_index == first;
    }
};

// The strings of one block, keyed by `table:index`, plus the wildcard rules and
// the range rules the file may use. A plain file holds one block, the main one.
class Translations {
public:
    [[nodiscard]] bool empty() const noexcept { return order_.empty() && patterns_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }
    [[nodiscard]] const std::string &code() const noexcept { return code_; }
    [[nodiscard]] const std::string &name() const noexcept { return name_; }

    // The translation for one string: an exact key first, then the wildcard
    // rules in the order the file wrote them, or null when none applies.
    [[nodiscard]] const std::string *find(std::uint16_t table, std::uint32_t index) const;
    [[nodiscard]] const std::string *find(std::uint64_t key) const;

    // Adds one string, replacing an earlier one with the same key.
    void add(std::uint16_t table, std::uint32_t index, std::string text);
    void add(std::uint64_t key, std::string text);
    // Adds a rule that matches many strings.
    void add(Pattern pattern, std::string text);

    // Every exact string, in the order it was added.
    [[nodiscard]] const std::vector<std::pair<std::uint64_t, std::string>> &entries() const noexcept { return order_; }
    [[nodiscard]] const std::vector<std::pair<Pattern, std::string>> &patterns() const noexcept { return patterns_; }

    void set_code(std::string code) { code_ = std::move(code); }
    void set_name(std::string name) { name_ = std::move(name); }

    // How many bytes the strings take in an arena, terminators included. A
    // pattern counts once per string it can match in a block of `tables`
    // strings each.
    [[nodiscard]] std::size_t arena_bytes(const std::vector<std::uint32_t> &table_sizes) const noexcept;
    // Upper bound when each exact string and rule is stored once and reused.
    [[nodiscard]] std::size_t arena_bytes() const noexcept;

    // Parses `text` (a whole file) and fills `blocks`: block entry -> its
    // strings. A file with no `[entry]` header goes to kMainEntry.
    [[nodiscard]] static std::map<std::uint32_t, Translations> parse_blocks(
        const std::string &text, const std::string &fallback_code);
    // Parses one block's worth, for a caller that has the text of a single
    // section.
    [[nodiscard]] static Translations parse(const std::string &text, const std::string &fallback_code);

    // Reads one file and splits it into its blocks. `error` is set, and nothing
    // is returned, when the file cannot be read.
    [[nodiscard]] static std::optional<std::map<std::uint32_t, Translations>> from_file(
        const std::filesystem::path &file, std::string &error, std::string *validated_contents = nullptr);

private:
    std::string code_;
    std::string name_;
    std::map<std::uint64_t, std::size_t> index_; // exact key -> position in order_
    std::vector<std::pair<std::uint64_t, std::string>> order_;
    std::vector<std::pair<Pattern, std::string>> patterns_;
};

// Every `*.lang` file in `directory`, sorted by language code. A file with a
// language line and no name takes its file name. Unreadable files are skipped.
[[nodiscard]] std::vector<Language> scan_languages(const std::filesystem::path &directory);

// What importing one translation file did.
struct TranslationImport {
    std::string code;            // the language's own code, or its file name
    std::string name;            // for the menu
    std::filesystem::path saved; // where it was copied
    std::string error;           // why it could not be imported; empty on success
};

// Reads `source` as a translation file and copies it into `folder` under its own
// language code (`<code>.lang`), so the loader finds it at the next start. The
// file is read first, so a file that is not a translation is refused; the folder
// is created when missing, and a file of the same code is replaced. Nothing is
// written when the source cannot be read.
[[nodiscard]] TranslationImport import_translation_file(
    const std::filesystem::path &source, const std::filesystem::path &folder);

} // namespace mhp3rd::text
