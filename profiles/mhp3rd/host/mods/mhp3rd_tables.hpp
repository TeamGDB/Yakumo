#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psprecomp {
class Runtime;
}

// Game tables mods can make longer (docs/MODDING.md, "Game tables"): tables
// indexed by an id the game has a fixed number of, such as the kinds of large
// monster, and the model sets their files come from. A mod that needs more
// rows (a new monster) extends the table: Yakumo moves it to guest memory of
// its own with room for the rows asked for, copies the game's rows into it,
// and points every instruction pair that forms the table's address at the
// copy. The units whose code changed leave the recompiled code and run
// through the JIT. A count the game compares the id with, where it has one,
// is raised with the table.
//
// Each table's references were traced (docs/MONSTERS.md) and are checked
// against the code before anything changes: on another executable, or when
// another mod changed the code there, the table is not moved. A table inside
// a code overlay is moved again each time its overlay loads; its copy keeps
// the rows mods wrote.
namespace mhp3rd::mods::tables {

struct Table {
    std::string name;
    std::string overlay;     // empty: the executable
    std::uint32_t address{}; // where the game keeps it
    std::uint32_t row{};     // bytes per row
    std::uint32_t rows{};    // rows the game has
};

// The tables this executable has, by name.
[[nodiscard]] const std::vector<Table> &all();
[[nodiscard]] const Table *find(std::string_view name);

// Where the table is now (its copy once extended), how many rows it has
// there, or nothing for a table not known.
struct Placement {
    std::uint32_t address{};
    std::uint32_t rows{};
};
[[nodiscard]] std::optional<Placement> placement(std::string_view name);

// Gives the table at least `rows` rows and returns where they are, or
// nothing (with the reason in `why`) when the table is not known, cannot grow
// that far, or its code is not as traced. The game's rows are copied (for an
// overlay's table, when the overlay loads); rows past them start at zero.
// Several mods may extend a table: it is as long as the longest asked for.
[[nodiscard]] std::optional<std::uint32_t> extend(
    psprecomp::Runtime &runtime, std::string_view name, std::uint32_t rows, std::string *why = nullptr);

// Whether extend() could give the table `rows` rows now, changing nothing;
// the reason in `why` when not. Whether guest memory is left for the copy is
// the one thing it does not tell.
[[nodiscard]] bool can_extend(
    psprecomp::Runtime &runtime, std::string_view name, std::uint32_t rows, std::string *why = nullptr);

// Moves the tables of overlays that have just loaded. Called after an
// overlay is copied to its slot and before the game runs it.
void overlay_loaded(psprecomp::Runtime &runtime);

// Writes, for tests, the game's code at every place the table's address and
// count are read, as traced, so extend() finds it as it would in the game.
void write_traced_code_for_tests(psprecomp::Runtime &runtime, std::string_view name);

// Forgets every table moved, so tests can start again.
void reset_for_tests();

} // namespace mhp3rd::mods::tables
