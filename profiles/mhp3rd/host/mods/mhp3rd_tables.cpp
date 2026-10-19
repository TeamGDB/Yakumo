#include "mods/mhp3rd_tables.hpp"

#include "mods/mhp3rd_code_mods.hpp"
#include "mods/mod_ini.hpp"

#include "kernel/kernel.hpp"
#include "overlays.hpp"

#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <vector>

namespace mhp3rd::mods::tables {
namespace {

// An instruction pair that forms an address in the table: lui at `hi`, and
// at `lo` an instruction whose signed immediate completes it, `offset` bytes
// past the table's start. Both words are the game's, as traced; they are
// only ever rewritten from these.
struct Reference {
    std::uint32_t hi;
    std::uint32_t hi_word;
    std::uint32_t lo;
    std::uint32_t lo_word;
    std::uint32_t offset{};
};

// An instruction whose immediate is the table's row count.
struct Count {
    std::uint32_t address;
    std::uint32_t word;
};

struct Definition {
    Table table;
    std::uint32_t most_rows; // what the index the game uses can reach
    std::vector<Reference> references;
    std::vector<Count> counts;
};

// Traced on NPJB-40001 (docs/MONSTERS.md).
const std::vector<Definition> &definitions() {
    static const std::vector<Definition> list{
        // The first file of each model set, u16 by set (em) number from 1:
        // 0x088564E4 reads it, 0x08856684 searches it for a file.
        {{"monster.models", "", 0x08982DD0u, 2u, 74u}, 255u,
            {{0x088564E8u, 0x3C020898u, 0x088564ECu, 0x24422DD0u},
                {0x08856684u, 0x3C020898u, 0x0885668Cu, 0x24452DD0u}},
            {{0x08856694u, 0x2407004Au}}},
        // The second file of each model set, read by 0x08856664.
        {{"monster.models.second", "", 0x08982E64u, 2u, 74u}, 255u,
            {{0x08856668u, 0x3C020898u, 0x0885666Cu, 0x24422E64u}}, {}},
        // The third file of each model set, read by 0x088F4BBC.
        {{"monster.models.third", "", 0x089FC040u, 2u, 74u}, 255u,
            {{0x088F4BC0u, 0x3C0208A0u, 0x088F4BC4u, 0x2442C040u}}, {}},
        // Each kind of large monster: its AI overlays with their descriptors
        // and how many slots it takes. 0x0A08CC60 reads it, and 0x0A08CE1C.
        {{"monster.kinds", "game_task", 0x0A17FE84u, 8u, 75u}, 256u,
            {{0x0A08CC64u, 0x3C020A18u, 0x0A08CC6Cu, 0x2442FE84u},
                {0x0A08CE38u, 0x3C030A18u, 0x0A08CE3Cu, 0x2463FE84u}},
            {{0x0A08CF10u, 0x28A2004Bu}}},
        // Where 0x0A090AE0, which makes a large monster, goes for each kind:
        // the code that builds that kind's object. A kind past the table's
        // rows gets an object of no kind, which stands still.
        {{"monster.constructors", "game_task", 0x0A19F440u, 4u, 75u}, 256u,
            {{0x0A090BE0u, 0x3C030A1Au, 0x0A090BE8u, 0x2463F440u}}, {{0x0A090B2Cu, 0x2E82004Bu}}},
        // A pointer to each kind's 16-byte records that look like its carving
        // points, read with no bound when the monster is killed (0x0A164F88)
        // and by 0x0A164914. 0 for a kind with none.
        {{"monster.carves", "game_task", 0x0A194B0Cu, 4u, 75u}, 256u,
            {{0x0A164918u, 0x3C020A19u, 0x0A164924u, 0x24424B0Cu},
                {0x0A164F90u, 0x3C020A19u, 0x0A164F94u, 0x24424B0Cu}},
            {}},
        // Where 0x0A08C8A4 goes for each kind: the code that gives the size
        // of its object.
        {{"monster.sizes", "game_task", 0x0A19F2D8u, 4u, 75u}, 256u,
            {{0x0A08C8B4u, 0x3C030A1Au, 0x0A08C8BCu, 0x2463F2D8u}}, {{0x0A08C8A8u, 0x2CA2004Bu}}},
        // Four more files of each kind (u16, 0xFFFF for none), loaded with its
        // model set on a quest. Read by the executable and the quest overlay.
        {{"monster.files", "game_task", 0x0A1834F0u, 8u, 75u}, 256u,
            {{0x088C8DF8u, 0x3C020A18u, 0x088C8E00u, 0x244234F0u}, {0x0891A94Cu, 0x3C020A18u, 0x0891A950u, 0x244234F0u},
                {0x0891A99Cu, 0x3C020A18u, 0x0891A9A4u, 0x245634F0u},
                {0x0891AA58u, 0x3C020A18u, 0x0891AA5Cu, 0x244234F0u},
                {0x0891B460u, 0x3C020A18u, 0x0891B464u, 0x244234F0u},
                {0x0891BE44u, 0x3C020A18u, 0x0891BE48u, 0x244234F0u},
                {0x0891BE98u, 0x3C020A18u, 0x0891BEA0u, 0x245634F0u},
                {0x0891BF4Cu, 0x3C020A18u, 0x0891BF50u, 0x244234F0u},
                {0x0891CE7Cu, 0x3C040A18u, 0x0891CE84u, 0x249634F0u},
                {0x0A098F8Cu, 0x3C060A18u, 0x0A098F90u, 0x24C634F0u},
                {0x0A0991E4u, 0x3C020A18u, 0x0A0991ECu, 0x244234F0u},
                {0x0A0992D0u, 0x3C020A18u, 0x0A0992D8u, 0x244234F0u},
                {0x0A0994E8u, 0x3C040A18u, 0x0A0994ECu, 0x248434F0u},
                {0x0A099678u, 0x3C020A18u, 0x0A09967Cu, 0x244234F0u},
                {0x0A099E14u, 0x3C020A18u, 0x0A099E1Cu, 0x244234F0u}},
            {}},
        // The event quests the game includes, offered at the counter where the
        // downloaded ones are when there are none (u16 ids, 0 after the
        // last). The loop at 0x0A0B1324 loads each into the next free download slot
        // (100 of them) and 0x0A0B11A8 lists them; both read the next id
        // through base + 2.
        {{"quest.events", "lobby_task", 0x0A188B1Cu, 2u, 42u}, 101u,
            {{0x0A0B1264u, 0x3C030A19u, 0x0A0B1278u, 0x94628B1Cu},
                {0x0A0B1294u, 0x3C020A19u, 0x0A0B1298u, 0x24428B1Eu, 2u},
                {0x0A0B1324u, 0x3C020A19u, 0x0A0B1328u, 0x94458B1Cu},
                {0x0A0B1338u, 0x3C020A19u, 0x0A0B133Cu, 0x24528B1Eu, 2u}},
            {}},
    };
    return list;
}

struct Moved {
    std::uint32_t address{}; // the copy
    std::uint32_t rows{};
    bool filled{};  // the game's rows are in the copy
    bool refused{}; // the code was not as traced; said once
};

std::map<std::string, Moved, std::less<>> &moved() {
    static std::map<std::string, Moved, std::less<>> map;
    return map;
}

const Definition *definition(std::string_view name) {
    for (const Definition &d : definitions())
        if (d.table.name == name) return &d;
    return nullptr;
}

std::uint32_t with_immediate(std::uint32_t word, std::uint32_t immediate) {
    return (word & 0xFFFF0000u) | (immediate & 0xFFFFu);
}

bool loaded(psprecomp::Runtime &runtime, const Definition &d) {
    if (d.table.overlay.empty()) return true;
    const std::optional<LoadedOverlay> overlay = overlay_at(runtime.memory(), d.table.address);
    return overlay && iequals(overlay->name, d.table.overlay);
}

// Code in an overlay is only there, and only checked, while it is loaded;
// the executable's is always there.
bool present(psprecomp::Runtime &runtime, const Definition &d, std::uint32_t address) {
    return address < 0x0A000000u || loaded(runtime, d);
}

// What a reference holds: the game's words, the ones for the copy, or
// something else (another executable, another mod's change).
enum class Code { Game, Ours, Other };
Code reference_state(psprecomp::Runtime &runtime, const Reference &r, std::uint32_t copy) {
    auto &memory = runtime.memory();
    if (!memory.contains(r.hi, 4u) || !memory.contains(r.lo, 4u)) return Code::Other;
    const std::uint32_t h = memory.load32(r.hi), l = memory.load32(r.lo);
    if (h == r.hi_word && l == r.lo_word) return Code::Game;
    const std::uint32_t target = copy + r.offset;
    if (copy != 0u && h == with_immediate(r.hi_word, (target + 0x8000u) >> 16u) &&
        l == with_immediate(r.lo_word, target))
        return Code::Ours;
    return Code::Other;
}
Code count_state(psprecomp::Runtime &runtime, const Count &c, std::uint32_t rows) {
    auto &memory = runtime.memory();
    if (!memory.contains(c.address, 4u)) return Code::Other;
    const std::uint32_t w = memory.load32(c.address);
    if (w == c.word) return Code::Game;
    if (rows != 0u && w == with_immediate(c.word, rows)) return Code::Ours;
    return Code::Other;
}

// Whether every reference present holds the game's words or `m`'s.
bool as_traced(psprecomp::Runtime &runtime, const Definition &d, const Moved &m) {
    for (const Reference &r : d.references)
        if (present(runtime, d, r.hi) && reference_state(runtime, r, m.address) == Code::Other) return false;
    for (const Count &c : d.counts)
        if (present(runtime, d, c.address) && count_state(runtime, c, m.rows) == Code::Other) return false;
    return true;
}

void rewrite(psprecomp::Runtime &runtime, std::uint32_t address, std::uint32_t word, const std::string &who) {
    runtime.memory().store32(address, word);
    std::vector<std::string> problems;
    (void)p3rd::retire_code(runtime, address, 4u, who, problems);
    for (const std::string &line : problems) std::cerr << "[mods] " << line << "\n";
}

// Points the code present at the copy: every reference that does not point
// there yet. The overlay's own recompiled code goes in first, so only the
// units that change leave it. `previous` is the copy the code may point at.
void repoint(psprecomp::Runtime &runtime, const Definition &d, const Moved &m, std::uint32_t previous = 0u) {
    if (!d.table.overlay.empty() && loaded(runtime, d)) {
        if (const std::optional<LoadedOverlay> overlay = overlay_at(runtime.memory(), d.table.address))
            (void)install_overlay_at(runtime, overlay->base);
    }
    const std::string who = "table " + d.table.name;
    for (const Reference &r : d.references) {
        if (!present(runtime, d, r.hi) || reference_state(runtime, r, m.address) == Code::Ours) continue;
        if (reference_state(runtime, r, previous) == Code::Other) continue;
        const std::uint32_t target = m.address + r.offset;
        rewrite(runtime, r.hi, with_immediate(r.hi_word, (target + 0x8000u) >> 16u), who);
        rewrite(runtime, r.lo, with_immediate(r.lo_word, target), who);
    }
    for (const Count &c : d.counts) {
        if (!present(runtime, d, c.address) || count_state(runtime, c, m.rows) == Code::Ours) continue;
        rewrite(runtime, c.address, with_immediate(c.word, m.rows), who);
    }
}

// The game's rows into the copy, once, from where the game keeps them.
void fill(psprecomp::Runtime &runtime, const Definition &d, Moved &m) {
    if (m.filled || !loaded(runtime, d)) return;
    const std::uint32_t length = d.table.row * d.table.rows;
    if (!runtime.memory().contains(d.table.address, length)) return;
    std::vector<std::uint8_t> rows(length);
    runtime.memory().copy_out(d.table.address, rows);
    runtime.memory().copy_in(m.address, rows);
    m.filled = true;
}

void refuse(const Definition &d, Moved &m, const char *why) {
    if (!m.refused) std::cerr << "[mods] table " << d.table.name << " is not moved: " << why << "\n";
    m.refused = true;
}

} // namespace

const std::vector<Table> &all() {
    static const std::vector<Table> list = [] {
        std::vector<Table> out;
        for (const Definition &d : definitions()) out.push_back(d.table);
        return out;
    }();
    return list;
}

const Table *find(std::string_view name) {
    const Definition *d = definition(name);
    return d != nullptr ? &d->table : nullptr;
}

std::optional<Placement> placement(std::string_view name) {
    const Definition *d = definition(name);
    if (d == nullptr) return std::nullopt;
    if (const auto found = moved().find(name); found != moved().end() && found->second.address != 0u)
        return Placement{found->second.address, found->second.rows};
    return Placement{d->table.address, d->table.rows};
}

std::optional<std::uint32_t> extend(
    psprecomp::Runtime &runtime, std::string_view name, std::uint32_t rows, std::string *why) {
    const auto refused = [why](const std::string &reason) -> std::optional<std::uint32_t> {
        if (why != nullptr) *why = reason;
        return std::nullopt;
    };
    const Definition *d = definition(name);
    if (d == nullptr) return refused("no such table");
    if (rows > d->most_rows) return refused("the game's index reaches " + std::to_string(d->most_rows) + " rows");
    Moved &m = moved()[d->table.name];
    if (rows <= std::max(m.rows, d->table.rows) && m.address != 0u) return m.address;
    if (rows <= d->table.rows && m.address == 0u) return d->table.address;
    if (!as_traced(runtime, *d, m)) return refused("the game's code that reads it is not as traced");

    const std::uint32_t length = rows * d->table.row;
    const std::int32_t uid = kernel().allocate_block("table " + d->table.name, 4u, length, 16u);
    const MemoryBlock *block = uid >= 0 ? kernel().find_block(uid) : nullptr;
    if (block == nullptr) return refused("no guest memory for it");
    std::vector<std::uint8_t> bytes(length, 0u);
    // A longer copy keeps what the shorter one held, the mods' rows too. The
    // old copy stays allocated: code compiled before may still read it.
    if (m.address != 0u) runtime.memory().copy_out(m.address, {bytes.data(), m.rows * d->table.row});
    runtime.memory().copy_in(block->address, bytes);
    Moved next{block->address, rows, m.filled, false};
    fill(runtime, *d, next);
    repoint(runtime, *d, next, m.address);
    m = next;
    std::cout << "[mods] table " << d->table.name << " moved to 0x" << std::hex << m.address << std::dec << " with "
              << rows << " rows\n";
    return m.address;
}

void overlay_loaded(psprecomp::Runtime &runtime) {
    for (auto &[name, m] : moved()) {
        const Definition *d = definition(name);
        if (d == nullptr || d->table.overlay.empty() || m.address == 0u || !loaded(runtime, *d)) continue;
        if (!as_traced(runtime, *d, m)) {
            refuse(*d, m, "the overlay's code that reads it is not as traced");
            continue;
        }
        fill(runtime, *d, m);
        repoint(runtime, *d, m);
    }
}

void reset_for_tests() {
    moved().clear();
}

} // namespace mhp3rd::mods::tables
