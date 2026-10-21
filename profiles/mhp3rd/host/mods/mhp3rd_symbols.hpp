#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psprecomp {
class GuestMemory;
}

// The game's known functions and data by name, for mods: the table in
// sdk/symbols.txt, compiled in. Native mods reach it through the
// "yakumo.symbols" service, scripts through yakumo.symbol().
//
// A function's entry carries a hash of its first 16 bytes as the game ships
// them. Functions of the executable are checked once, at start, before any
// mod writes code; those of an overlay when they are looked up while their
// overlay is loaded. A function whose bytes differ is not given out, so a mod
// never hooks the wrong code on another version of the game.
namespace mhp3rd::mods::symbols {

struct Symbol {
    std::string name;
    std::string overlay; // empty: the executable
    std::string about;
    std::uint32_t address{};
    bool function{};
    std::optional<std::uint32_t> check; // functions only
};

// FNV-1a, 32-bit.
[[nodiscard]] std::uint32_t fnv1a(std::span<const std::uint8_t> bytes);

// The entries of a table in symbols.txt's format. Lines that do not read are
// left out, each with a line in `problems` when given.
[[nodiscard]] std::vector<Symbol> parse(std::string_view text, std::vector<std::string> *problems = nullptr);

// The compiled-in table.
[[nodiscard]] const std::vector<Symbol> &all();

// Checks the executable's functions against `memory`, once the executable is
// loaded and before mods change it. Returns how many were refused.
std::size_t verify(const psprecomp::GuestMemory &memory);

// The entry called `name`, or null when there is none or it was refused. An
// overlay's function is checked now when its overlay is loaded.
[[nodiscard]] const Symbol *find(std::string_view name, const psprecomp::GuestMemory *memory);

// The entry at exactly `address`, or null.
[[nodiscard]] const Symbol *at(std::uint32_t address);

// False when `address` in `overlay` (empty: the executable) is a known
// function whose bytes in `memory` are not the ones it was traced from: a
// hook there, however the mod found the address, would hook other code.
// True for addresses the table does not know.
[[nodiscard]] bool matches(std::uint32_t address, std::string_view overlay, const psprecomp::GuestMemory &memory);

// Tests: a table in place of the compiled-in one (checks made for stand-in
// code), and back to the compiled-in table with nothing refused.
void use_table_for_tests(std::vector<Symbol> table);
void reset_for_tests();

} // namespace mhp3rd::mods::symbols
