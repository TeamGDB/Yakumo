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

// The NUL-terminated string at an address.
std::string read_text_at(const Memory &memory, std::uint32_t address) {
    std::string text;
    for (std::uint32_t i = 0; i < 256u; ++i) {
        const char c = static_cast<char>(memory.load8(address + i));
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

// The dialogue shape: (id, offset) at the top, each offset a sub-block of
// (kind, offset), each offset a string from the sub-block start.
void write_dialogue(Memory &memory, std::uint32_t at, int blocks, int entries_per_block) {
    // Layout: the top (id, offset) table, then each sub-block, then the strings.
    // Every offset is relative to the base the game adds it to (the top table
    // for a block, the block for a string).
    const auto block_size = static_cast<std::uint32_t>(entries_per_block) * 8u + 16u +
                            static_cast<std::uint32_t>(entries_per_block) * 24u;
    std::uint32_t cursor = static_cast<std::uint32_t>(blocks) * 8u + 16u;
    for (int id = 0; id < blocks; ++id) {
        memory.store32(at + id * 8u, static_cast<std::uint32_t>(id));
        memory.store32(at + id * 8u + 4u, cursor);
        cursor += block_size;
    }
    memory.store32(at + blocks * 8u, 0xFFFFFFFFu);
    for (int id = 0; id < blocks; ++id) {
        const std::uint32_t relative = memory.load32(at + id * 8u + 4u);
        const std::uint32_t block = at + relative;
        const std::uint32_t strings = block + static_cast<std::uint32_t>(entries_per_block) * 8u + 16u;
        for (int k = 0; k < entries_per_block; ++k) {
            const std::string text = "d" + std::to_string(id) + "-" + std::to_string(k);
            const std::uint32_t string = strings + static_cast<std::uint32_t>(k) * 16u;
            for (std::size_t i = 0; i <= text.size(); ++i)
                memory.store8(string + static_cast<std::uint32_t>(i),
                              i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0u);
            memory.store32(block + k * 8u, 0u);
            memory.store32(block + k * 8u + 4u, string - block);
        }
        memory.store32(block + entries_per_block * 8u, 0xFFFFFFFFu);
    }
}

// The dialogue's ids need not start at 0: one archive entry is numbered from
// where the previous left off (a block of 17 packs ids 0..16, the next 17..23).
void test_apply_dialogue_offset_ids() {
    Memory memory(kBase, kSize);
    const std::uint32_t at = kBase + 0x2000u;
    // Two blocks whose ids are 17 and 18, like entry 4290.
    write_dialogue(memory, at, 2, 3);
    memory.store32(at + 0u, 17u);
    memory.store32(at + 8u, 18u);

    Translations t;
    t.add(17u, 0u, "primeiro");
    t.add(18u, 1u, "segundo");

    Arena arena{at + 0x8000u, at + 0x8000u + 0x400u};
    std::size_t used = 0u;
    const std::uint32_t applied = mhp3rd::text::apply_dialogue(memory, at, t, arena, used);
    check(applied == 2u, "an id that does not start at 0 is found");
    const std::uint32_t block0 = at + memory.load32(at + 4u);
    const std::uint32_t s0 = block0 + memory.load32(block0 + 4u);
    check(read_text_at(memory, s0) == "primeiro", "the first block's string with its own id");
}

void test_apply_dialogue() {
    Memory memory(kBase, kSize);
    const std::uint32_t at = kBase + 0x1000u;
    write_dialogue(memory, at, 2, 3);

    Translations t;
    t.add(0u, 1u, "olá");
    t.add(1u, 2u, "adeus");
    t.add(5u, 0u, "não existe");

    Arena arena{at + 0x8000u, at + 0x8000u + 0x400u};
    std::size_t used = 0u;
    const std::uint32_t applied = mhp3rd::text::apply_dialogue(memory, at, t, arena, used);
    check(applied == 2u, "two dialogue strings are replaced");
    // Read one back the way the game would: base plus the relative offset.
    const std::uint32_t block0 = at + memory.load32(at + 4u);
    const std::uint32_t string0 = block0 + memory.load32(block0 + 1u * 8u + 4u);
    check(read_text_at(memory, string0) == "olá", "the first string reads back translated");
    const std::uint32_t block1 = at + memory.load32(at + 8u + 4u);
    const std::uint32_t string1 = block1 + memory.load32(block1 + 2u * 8u + 4u);
    check(read_text_at(memory, string1) == "adeus", "the second block's string too");
    const std::uint32_t untouched = block0 + memory.load32(block0 + 0u * 8u + 4u);
    check(read_text_at(memory, untouched) == "d0-0", "a string with no translation keeps the game's text");
}

// A quest file: an array of record offsets at the top, then each record's
// string table (here a run of entry-relative offsets) and the strings.
void write_quest(Memory &memory, std::uint32_t at) {
    memory.store32(at + 0u, 0x100u);  // record 0
    memory.store32(at + 4u, 0x200u);  // record 1
    // Record 0's six fields, at their offsets, then record 1's.
    const char *titles[] = {"Title", "Objective", "Result", "Body", "Monsters", "Client"};
    for (std::uint32_t field = 0; field < 6u; ++field) {
        const std::uint32_t string = 0x400u + field * 0x20u;
        memory.store32(at + 0x100u + field * 4u, string);
        const std::string text = std::string(titles[field]) + "0";
        for (std::size_t i = 0; i <= text.size(); ++i)
            memory.store8(at + string + static_cast<std::uint32_t>(i),
                          i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0u);
    }
}

void test_apply_quest() {
    Memory memory(kBase, kSize);
    const std::uint32_t at = kBase + 0x4000u;
    write_quest(memory, at);

    // The keys are `ref:offset`: the word that holds the offset, and the offset.
    Translations t;
    t.add(0x100u, 0x400u, "Titulo");      // record 0's title
    t.add(0x104u, 0x420u, "Um objetivo bem mais longo");  // any length
    t.add(0x108u, 0x999u, "deslocado");   // the word does not hold 0x999

    Arena arena{at + 0x8000u, at + 0x8000u + 0x400u};
    std::size_t used = 0u;
    const std::uint32_t applied = mhp3rd::text::apply_quest(memory, at, t, arena, used);
    check(applied == 2u, "two quest fields are replaced");
    const std::uint32_t title = at + memory.load32(at + 0x100u);
    check(read_text_at(memory, title) == "Titulo", "the title is repointed and reads back");
    const std::uint32_t objective = at + memory.load32(at + 0x104u);
    check(read_text_at(memory, objective) == "Um objetivo bem mais longo",
          "a longer translation is stored whole");
    check(memory.load32(at + 0x108u) == 0x999u || read_text_at(memory, at + memory.load32(at + 0x108u)) != "deslocado",
          "a word that no longer matches is left alone");
    const std::uint32_t untouched = at + memory.load32(at + 0x10Cu);
    check(read_text_at(memory, untouched) == "Body0", "a field with no translation keeps the game's text");
}

// A quest file is in RAM more than once (the list's buffer, the quest's), and
// is read again every time its screen opens; every copy shares one arena slice.
void test_apply_quest_copies() {
    Memory memory(kBase, kSize);
    const std::uint32_t first = kBase + 0x4000u;
    const std::uint32_t second = kBase + 0x20000u;
    write_quest(memory, first);
    write_quest(memory, second);

    Translations t;
    t.add(0x100u, 0x400u, "Titulo");

    Arena slice{first + 0x10000u, first + 0x10000u + 0x400u};
    std::uint32_t applied = 0u;
    for (const std::uint32_t base : {first, second}) {
        std::size_t used = 0u;
        applied += mhp3rd::text::apply_quest(memory, base, t, slice, used);
    }
    check(applied == 2u, "every copy of the file is translated");
    check(read_text_at(memory, first + memory.load32(first + 0x100u)) == "Titulo", "the first copy reads back");
    check(read_text_at(memory, second + memory.load32(second + 0x100u)) == "Titulo", "the second copy reads back");
}

} // namespace

int main() {
    test_parse();
    test_blocks_and_rules();
    test_apply();
    test_apply_limited_arena();
    test_apply_before_load();
    test_apply_dialogue();
    test_apply_dialogue_offset_ids();
    test_apply_quest();
    test_apply_quest_copies();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "text tests passed\n";
    return 0;
}
