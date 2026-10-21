#include "mods/mhp3rd_texts.hpp"

#include "mods/mod_native.hpp"

#include "psprecomp/runtime.hpp"
#include "yakumo/mod.h"

#include <iostream>
#include <map>
#include <vector>

namespace mhp3rd::mods::texts {
namespace {

constexpr std::uint32_t kBlock = 0x08A40640u; // the main text block, loaded at start
constexpr std::uint32_t kTables = 0u;         // table t's offset is header word t (the game's reader is passed t - 2)
constexpr std::uint32_t kEnd = 0xFFFFFFFFu;   // the offset that ends a table
constexpr std::uint32_t kMostRows = 4096u;    // more than any of the game's tables holds

struct Added {
    std::string text;
    std::function<void(std::uint32_t)> placed;
    bool told{};
};

struct Copy {
    std::uint32_t address{};
    std::uint32_t size{};
    std::uint32_t original{}; // the game's table it was copied from, and its rows
    std::uint32_t rows{};
    bool stale{}; // strings were added since
};

struct State {
    std::map<std::uint32_t, std::vector<Added>> added; // by table
    std::map<std::uint32_t, Copy> copies;
    ymod_mod *host{};
};

State &state() {
    static State s;
    return s;
}

std::uint32_t allocate(psprecomp::Runtime &runtime, std::uint32_t size) {
    State &s = state();
    native::attach(runtime);
    if (s.host == nullptr) s.host = native::add_mod("yakumo.texts", "Text from mods", "");
    const auto *memory = static_cast<const ymod_memory *>(native::host()->service(s.host, YMOD_MEMORY, 1u));
    return memory != nullptr ? memory->allocate(s.host, size, 16u) : 0u;
}

// The game's table as it is now: its address and rows, or nothing when the
// block is not loaded (the header or the table outside RAM, or no end).
struct Table {
    std::uint32_t address{};
    std::uint32_t rows{};
};
std::optional<Table> game_table(psprecomp::Runtime &runtime, std::uint32_t table) {
    auto &memory = runtime.memory();
    const std::uint32_t entry = kBlock + kTables + 4u * table;
    if (!memory.contains(entry, 4u)) return std::nullopt;
    const std::uint32_t address = kBlock + memory.load32(entry);
    if (address == kBlock || !memory.contains(address, 4u)) return std::nullopt;
    for (std::uint32_t row = 0; row < kMostRows; ++row) {
        if (!memory.contains(address + 4u * row, 4u)) return std::nullopt;
        if (memory.load32(address + 4u * row) == kEnd) return Table{address, row};
    }
    return std::nullopt;
}

void place(psprecomp::Runtime &runtime, std::uint32_t table, std::vector<Added> &strings) {
    State &s = state();
    const std::optional<Table> now = game_table(runtime, table);
    if (!now) return;
    Copy &copy = s.copies[table];
    const bool ours = copy.address != 0u && now->address == copy.address;
    if (ours && !copy.stale) return; // in place since last time
    // From the game's table: the one the copy was made from when the header
    // still points at the copy, or the one the game has just loaded.
    const Table game = ours ? Table{copy.original, copy.rows} : *now;
    copy.original = game.address;
    copy.rows = game.rows;
    copy.stale = false;
    auto &memory = runtime.memory();
    const std::uint32_t rows = game.rows + static_cast<std::uint32_t>(strings.size());
    std::uint32_t size = 4u * (rows + 1u);
    for (const Added &a : strings) size += (static_cast<std::uint32_t>(a.text.size()) + 4u) & ~3u;
    if (copy.address == 0u || copy.size < size) {
        copy.address = allocate(runtime, size);
        copy.size = size;
        if (copy.address == 0u) {
            std::cerr << "[mods] text table " << table << " cannot be made longer\n";
            return;
        }
    }
    // Through a scratch list: the copy may be written over the table it is read from.
    std::vector<std::uint32_t> texts(game.rows);
    for (std::uint32_t row = 0; row < game.rows; ++row)
        texts[row] = game.address + memory.load32(game.address + 4u * row);
    for (std::uint32_t row = 0; row < game.rows; ++row)
        memory.store32(copy.address + 4u * row, texts[row] - copy.address);
    std::uint32_t at = copy.address + 4u * (rows + 1u);
    for (std::size_t i = 0; i < strings.size(); ++i) {
        memory.store32(copy.address + 4u * (game.rows + static_cast<std::uint32_t>(i)), at - copy.address);
        for (const char c : strings[i].text) memory.store8(at++, static_cast<std::uint8_t>(c));
        memory.store8(at++, 0u);
        while ((at & 3u) != 0u) memory.store8(at++, 0u);
    }
    memory.store32(copy.address + 4u * rows, kEnd);
    memory.store32(kBlock + kTables + 4u * table, copy.address - kBlock);
    for (std::size_t i = 0; i < strings.size(); ++i) {
        if (strings[i].told || !strings[i].placed) continue;
        strings[i].told = true;
        strings[i].placed(game.rows + static_cast<std::uint32_t>(i));
    }
}

} // namespace

void add(std::uint32_t table, std::string text, std::function<void(std::uint32_t index)> placed) {
    State &s = state();
    s.added[table].push_back({std::move(text), std::move(placed)});
    // Copied again with the new string at the next frame.
    if (auto found = s.copies.find(table); found != s.copies.end()) found->second.stale = true;
}

void frame(psprecomp::Runtime &runtime) {
    for (auto &[table, strings] : state().added) place(runtime, table, strings);
}

void reset_for_tests() {
    state() = {};
}

} // namespace mhp3rd::mods::texts
