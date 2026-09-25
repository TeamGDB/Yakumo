// The game-text translations (host/text/): reading a translation file, and
// applying one to a text block on a buffer standing for guest memory. No game
// data: the block and the strings here are made up.
#include "text/language.hpp"
#include "text/translation.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
using mhp3rd::text::Arena;
using mhp3rd::text::Translations;

int failures{};

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

// Guest memory as the apply() template needs it: the same methods
// psprecomp::GuestMemory has, over a plain buffer.
class Memory {
public:
    Memory(std::uint32_t base, std::size_t size) : base_(base), bytes_(size, 0u) {}

    [[nodiscard]] bool contains(std::uint32_t address, std::size_t length) const {
        return address >= base_ && static_cast<std::size_t>(address - base_) + length <= bytes_.size();
    }
    [[nodiscard]] std::uint8_t load8(std::uint32_t address) const {
        return contains(address, 1u) ? bytes_[address - base_] : 0u;
    }
    [[nodiscard]] std::uint32_t load32(std::uint32_t address) const {
        return static_cast<std::uint32_t>(load8(address)) | (static_cast<std::uint32_t>(load8(address + 1u)) << 8u) |
               (static_cast<std::uint32_t>(load8(address + 2u)) << 16u) |
               (static_cast<std::uint32_t>(load8(address + 3u)) << 24u);
    }
    void store8(std::uint32_t address, std::uint8_t value) {
        if (contains(address, 1u)) bytes_[address - base_] = value;
    }
    void store32(std::uint32_t address, std::uint32_t value) {
        store8(address, static_cast<std::uint8_t>(value));
        store8(address + 1u, static_cast<std::uint8_t>(value >> 8u));
        store8(address + 2u, static_cast<std::uint8_t>(value >> 16u));
        store8(address + 3u, static_cast<std::uint8_t>(value >> 24u));
    }

private:
    std::uint32_t base_;
    std::vector<std::uint8_t> bytes_;
};

constexpr std::uint32_t kBase = 0x08800000u;
constexpr std::size_t kSize = 0x02000000u;

// The game lays a table out as offsets from the table start, an end marker,
// then the strings.
void write_table(Memory &memory, std::uint32_t at, const std::vector<std::string> &strings) {
    const auto count = static_cast<std::uint32_t>(strings.size());
    std::uint32_t text = (count + 1u) * 4u;
    for (std::uint32_t i = 0; i < count; ++i) {
        memory.store32(at + i * 4u, text);
        for (std::size_t c = 0; c <= strings[i].size(); ++c)
            memory.store8(at + text + static_cast<std::uint32_t>(c),
                          c < strings[i].size() ? static_cast<std::uint8_t>(strings[i][c]) : 0u);
        text += static_cast<std::uint32_t>(strings[i].size() + 1u);
    }
    memory.store32(at + count * 4u, 0xFFFFFFFFu);
}

// A text block with menu text at table 2 and item names at table 3, like the
// game's own.
void write_text(Memory &memory) {
    const std::uint32_t menus = mhp3rd::text::kMainTextBlock + 0x1000u;
    const std::uint32_t items = mhp3rd::text::kMainTextBlock + 0x3000u;
    memory.store32(mhp3rd::text::kMainTextBlock + 2u * 4u, menus - mhp3rd::text::kMainTextBlock);
    memory.store32(mhp3rd::text::kMainTextBlock + 3u * 4u, items - mhp3rd::text::kMainTextBlock);
    write_table(memory, menus, {"", "Qty", "Cancel", "Yes", "No"});
    write_table(memory, items, {"", "Guide", "Tonic"});
}

// Reads an entry the way the game does: table start plus the entry's offset.
std::string read_entry(const Memory &memory, std::uint16_t table, std::uint32_t entry) {
    const std::uint32_t at = mhp3rd::text::kMainTextBlock + memory.load32(mhp3rd::text::kMainTextBlock + table * 4u);
    const std::uint32_t string = at + memory.load32(at + entry * 4u);
    std::string text;
    for (std::uint32_t i = 0; i < 256u; ++i) {
        const char c = static_cast<char>(memory.load8(string + i));
        if (c == '\0') break;
        text += c;
    }
    return text;
}

void test_parse() {
    // The old shape (no [entry]): everything belongs to the main block, and
    // escapes are understood.
    const std::string text =
        "# a comment\n"
        "; another\n"
        "language = pt-BR\n"
        "name = PortuguÃªs (Brasil)\n"
        "\n"
        "2:20 = Cancelar\n"
        "2:21 = Sim \\#1\n"
        "3:1 = Linha 1\\nLinha 2\n"
        "not a key = ignored\n"
        "2:20 = Duplicado\n";
    const Translations t = Translations::parse(text, "xx");
    check(t.code() == "pt-BR", "the language line wins");
    check(t.name() == "PortuguÃªs (Brasil)", "the name line is read");
    check(t.size() == 3u, "comments and malformed lines are skipped");
    check(t.find(2u, 20u) != nullptr && *t.find(2u, 20u) == "Duplicado", "a repeated key keeps the last value");
    check(t.find(2u, 21u) != nullptr && *t.find(2u, 21u) == "Sim #1", "\\# unescapes");
    check(t.find(3u, 1u) != nullptr && *t.find(3u, 1u) == "Linha 1\nLinha 2", "\\n unescapes");
    check(t.find(2u, 99u) == nullptr, "an unknown key is absent");
    check(t.arena_bytes() == t.find(2u, 20u)->size() + t.find(2u, 21u)->size() + t.find(3u, 1u)->size() + 3u,
          "the arena size counts every terminator");
}

void test_blocks_and_rules() {
    // The grouped shape: [entry] sections, a range and a wildcard.
    const std::string text =
        "language = pt-BR\n"
        "name = Teste\n"
        "\n"
        "[16]\n"
        "2:20 = Cancelar\n"
        "3:1-3 = Faixa\n"
        "2:* = Tudo\n"
        "\n"
        "[2835]\n"
        "2:129 = Bem-vindo\n";
    const auto blocks = Translations::parse_blocks(text, "xx");
    check(blocks.size() == 2u, "two blocks are read");
    const Translations &main = blocks.at(16);
    check(main.find(2u, 20u) != nullptr && *main.find(2u, 20u) == "Cancelar", "an exact key in its block");
    check(main.find(3u, 2u) != nullptr && *main.find(3u, 2u) == "Faixa", "a range matches inside");
    check(main.find(3u, 4u) == nullptr, "a range stops at its end");
    check(main.find(2u, 999u) != nullptr && *main.find(2u, 999u) == "Tudo", "a wildcard matches any index");
    check(main.find(3u, 999u) == nullptr, "a wildcard of one table does not match another");
    check(blocks.at(2835).find(2u, 129u) != nullptr, "the other block has its own keys");
    check(blocks.at(2835).find(2u, 20u) == nullptr, "and does not see the first block's");
}

void test_apply() {
    Memory memory(kBase, kSize);
    write_text(memory);

    Translations t;
    t.add(2u, 1u, "Qtd");        // shorter than "Qty"
    t.add(2u, 2u, "Cancelar");   // longer than "Cancel"
    t.add(3u, 2u, "TÃ´nico");     // longer than "Tonic"
    t.add(2u, 99u, "fora");      // the table has no such entry

    const Arena arena{mhp3rd::text::kMainTextBlock + 0x8000u, mhp3rd::text::kMainTextBlock + 0x8000u + 0x400u};
    const mhp3rd::text::ApplyResult result = mhp3rd::text::apply(memory, mhp3rd::text::kMainTextBlock, t, arena);
    check(result.block, "the block is recognised");
    check(result.applied == 3u, "three entries are replaced");
    check(result.missing == 1u, "the entry the table lacks is counted");
    check(result.skipped == 0u, "nothing is skipped with room to spare");
    check(read_entry(memory, 2u, 1u) == "Qtd", "a shorter translation replaces in place");
    check(read_entry(memory, 2u, 2u) == "Cancelar", "a longer translation is read back whole");
    check(read_entry(memory, 3u, 2u) == "TÃ´nico", "a value with a multibyte character survives");
    check(read_entry(memory, 2u, 3u) == "Yes", "an entry with no translation falls back");
    check(read_entry(memory, 3u, 1u) == "Guide", "another table's entries fall back too");
}

void test_apply_limited_arena() {
    Memory memory(kBase, kSize);
    write_text(memory);

    Translations t;
    t.add(2u, 1u, "Qtd");
    t.add(2u, 2u, "Cancelar");  // does not fit the arena below

    const Arena arena{mhp3rd::text::kMainTextBlock + 0x8000u, mhp3rd::text::kMainTextBlock + 0x8000u + 5u};
    const mhp3rd::text::ApplyResult result = mhp3rd::text::apply(memory, mhp3rd::text::kMainTextBlock, t, arena);
    check(result.applied == 1u && result.skipped == 1u, "an entry with no room is skipped");
    check(read_entry(memory, 2u, 1u) == "Qtd", "the entry that did fit is applied");
    check(read_entry(memory, 2u, 2u) == "Cancel", "the skipped entry keeps the game's text");
}

void test_apply_before_load() {
    Memory memory(kBase, kSize);  // no text written: the block is all zeros

    Translations t;
    t.add(2u, 2u, "Qtd");
    const Arena arena{mhp3rd::text::kMainTextBlock + 0x8000u, mhp3rd::text::kMainTextBlock + 0x8000u + 0x400u};
    const mhp3rd::text::ApplyResult result = mhp3rd::text::apply(memory, mhp3rd::text::kMainTextBlock, t, arena);
    check(!result.block && result.applied == 0u, "nothing is applied before the game loads its text");
}

} // namespace

int main() {
    test_parse();
    test_blocks_and_rules();
    test_apply();
    test_apply_limited_arena();
    test_apply_before_load();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "text tests passed\n";
    return 0;
}
