#include "mods/mhp3rd_symbols.hpp"

#include "mods/mod_ini.hpp"
#include "overlays.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <charconv>
#include <iostream>
#include <set>
#include <sstream>

namespace mhp3rd::mods::symbols {
namespace {

// sdk/symbols.txt, one string per line (CMake writes it at configure time).
constexpr const char *kTable[] = {
#include "mod_symbols.inc"
};

constexpr std::uint32_t kCheckedBytes = 16u;

std::set<std::string> &refused() {
    static std::set<std::string> names;
    return names;
}

bool hex_number(std::string_view text, std::uint32_t &out) {
    if (text.starts_with("0x") || text.starts_with("0X")) text.remove_prefix(2);
    if (text.empty() || text.size() > 8u) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out, 16);
    return error == std::errc{} && end == text.data() + text.size();
}

bool bytes_match(const Symbol &symbol, const psprecomp::GuestMemory &memory) {
    if (!symbol.check || !memory.contains(symbol.address, kCheckedBytes)) return !symbol.check;
    std::array<std::uint8_t, kCheckedBytes> bytes{};
    memory.copy_out(symbol.address, bytes);
    return fnv1a(bytes) == *symbol.check;
}

} // namespace

std::uint32_t fnv1a(std::span<const std::uint8_t> bytes) {
    std::uint32_t hash = 0x811C9DC5u;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x01000193u;
    }
    return hash;
}

std::vector<Symbol> parse(std::string_view text, std::vector<std::string> *problems) {
    std::vector<Symbol> out;
    std::set<std::string> names;
    std::size_t number = 0u;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1u);
        ++number;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos || line[first] == '#') continue;
        std::istringstream in{std::string(line)};
        std::string kind, name, address, overlay, check;
        in >> kind >> name >> address >> overlay >> check;
        std::string about;
        std::getline(in >> std::ws, about);
        Symbol symbol;
        symbol.name = name;
        symbol.about = about;
        symbol.function = kind == "function";
        symbol.overlay = overlay == "-" ? std::string() : overlay;
        std::uint32_t hash = 0u;
        const bool kind_ok = symbol.function || kind == "data";
        const bool check_ok = symbol.function ? hex_number(check, hash) : check == "-";
        const bool ok = kind_ok && !name.empty() && !overlay.empty() && check_ok &&
            hex_number(address, symbol.address) && (!symbol.function || (symbol.address & 3u) == 0u) &&
            !names.contains(name);
        if (!ok) {
            if (problems != nullptr) problems->push_back("line " + std::to_string(number) + ": " + std::string(line));
            continue;
        }
        if (symbol.function) symbol.check = hash;
        names.insert(name);
        out.push_back(std::move(symbol));
    }
    return out;
}

namespace {
std::optional<std::vector<Symbol>> &test_table() {
    static std::optional<std::vector<Symbol>> table;
    return table;
}
} // namespace

const std::vector<Symbol> &all() {
    if (test_table()) return *test_table();
    static const std::vector<Symbol> table = [] {
        std::string text;
        for (const char *line : kTable) {
            text += line;
            text += '\n';
        }
        return parse(text);
    }();
    return table;
}

std::size_t verify(const psprecomp::GuestMemory &memory) {
    refused().clear();
    std::size_t checked = 0u;
    for (const Symbol &symbol : all()) {
        if (!symbol.function || !symbol.overlay.empty()) continue;
        ++checked;
        if (!bytes_match(symbol, memory)) refused().insert(symbol.name);
    }
    if (!refused().empty())
        std::cout << "[mods] symbols: " << refused().size() << " of " << checked
                  << " functions differ from the game they were traced in and are not given to mods\n";
    return refused().size();
}

const Symbol *find(std::string_view name, const psprecomp::GuestMemory *memory) {
    for (const Symbol &symbol : all()) {
        if (symbol.name != name) continue;
        if (refused().contains(symbol.name)) return nullptr;
        if (symbol.function && !symbol.overlay.empty() && memory != nullptr) {
            const std::optional<LoadedOverlay> loaded = overlay_at(*memory, symbol.address);
            if (loaded && iequals(loaded->name, symbol.overlay) && !bytes_match(symbol, *memory)) return nullptr;
        }
        return &symbol;
    }
    return nullptr;
}

const Symbol *at(std::uint32_t address) {
    for (const Symbol &symbol : all())
        if (symbol.address == address && !refused().contains(symbol.name)) return &symbol;
    return nullptr;
}

bool matches(std::uint32_t address, std::string_view overlay, const psprecomp::GuestMemory &memory) {
    for (const Symbol &symbol : all()) {
        if (!symbol.function || symbol.address != address || !iequals(symbol.overlay, overlay)) continue;
        return !refused().contains(symbol.name) && bytes_match(symbol, memory);
    }
    return true;
}

void use_table_for_tests(std::vector<Symbol> table) {
    test_table() = std::move(table);
    refused().clear();
}

void reset_for_tests() {
    refused().clear();
    test_table().reset();
}

} // namespace mhp3rd::mods::symbols
