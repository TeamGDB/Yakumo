#include "mods/transmog_store.hpp"

#include "mods/mod_ini.hpp"
#include "psprecomp/common.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <set>

namespace mhp3rd::mods::transmog {
namespace {

constexpr std::size_t kMaxName = 48u;

void validate_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxName || trim(name) != name)
        throw psprecomp::Error("Transmog preset name must contain 1 to 48 characters without leading or trailing spaces.");
    for (const unsigned char character : name)
        if (!Store::name_character(character))
            throw psprecomp::Error("Transmog preset name contains an unsupported character; use printable text without quotes, brackets or equals signs.");
}

std::pair<unsigned, unsigned> read_appearance(const std::string &value, const std::string &context) {
    const auto separator = value.find(':');
    unsigned id{}, variant{};
    if (separator == std::string::npos)
        throw psprecomp::Error("Invalid transmog " + context + "='" + value + "'; use default or id:variant.");
    const auto [id_end, id_error] = std::from_chars(value.data(), value.data() + separator, id);
    const auto [variant_end, variant_error] = std::from_chars(value.data() + separator + 1u, value.data() + value.size(), variant);
    if (id_error != std::errc{} || id_end != value.data() + separator || id > 65535u ||
        variant_error != std::errc{} || variant_end != value.data() + value.size() || variant > 255u)
        throw psprecomp::Error("Invalid transmog " + context + "='" + value + "'; use a decimal id from 0 to 65535 and a valid variant after the colon.");
    return {id, variant};
}

void validate_set(const Set &set) {
    for (std::size_t slot = 0; slot < set.armor.size(); ++slot)
        if (set.armor[slot] && set.armor[slot]->gender > 1u)
            throw psprecomp::Error("Invalid " + std::string(kSlots[slot]) + " donor gender; expected 0 (male) or 1 (female).");
    if (set.weapon) {
        bool known{};
        for (const auto &weapon : kWeaponClasses) known = known || weapon.kind == set.weapon->kind;
        if (!known || set.weapon->id == 0u)
            throw psprecomp::Error("Invalid weapon appearance id/class; choose a nonzero id in a supported weapon class.");
    }
}

Set read_set(const IniFile &ini, const std::string &section) {
    Set result{};
    for (std::size_t slot = 0; slot < kSlots.size(); ++slot) {
        const std::string value = ini.get(section, kSlots[slot]);
        if (value == "default") continue;
        const auto [id, gender] = read_appearance(value, "[" + section + "] " + kSlots[slot]);
        result.armor[slot] = ArmorAppearance{static_cast<std::uint16_t>(id), static_cast<std::uint8_t>(gender)};
    }
    const std::string weapon = ini.get(section, "Weapon");
    if (weapon != "default") {
        const auto [id, kind] = read_appearance(weapon, "[" + section + "] Weapon");
        result.weapon = WeaponAppearance{static_cast<std::uint16_t>(id), static_cast<std::uint8_t>(kind)};
    }
    validate_set(result);
    std::set<std::string> keys;
    for (const auto &[key, value] : ini.entries(section)) {
        const std::string folded = lower(key);
        bool known = folded == "weapon" || (folded == "name" && section.starts_with("preset:"));
        for (const auto *slot : kSlots) known = known || folded == lower(slot);
        if (!known || !keys.insert(folded).second)
            throw psprecomp::Error("Unknown or duplicate transmog key '" + key + "' in [" + section + "].");
    }
    return result;
}

void write_set(std::ostream &output, const Set &set) {
    for (std::size_t slot = 0; slot < kSlots.size(); ++slot)
        output << kSlots[slot] << '=' << (set.armor[slot] ? std::to_string(set.armor[slot]->id) + ':' +
            std::to_string(set.armor[slot]->gender) : "default") << '\n';
    output << "Weapon=" << (set.weapon ? std::to_string(set.weapon->id) + ':' + std::to_string(set.weapon->kind) : "default") << '\n';
}

} // namespace

bool Store::name_character(char32_t character) {
    return character >= 0x20u && character <= 0x7Eu && character != '"' && character != '[' &&
           character != ']' && character != '=';
}

void Store::initialize(const std::filesystem::path &path) {
    path_ = path;
    load_valid_ = false;
    Set active{};
    Presets presets;
    if (std::filesystem::exists(path)) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw psprecomp::Error("Cannot open transmog file " + to_utf8(path) + "; check file permissions.");
        const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (input.bad()) throw psprecomp::Error("Cannot finish reading transmog file " + to_utf8(path) + "; check the storage device and retry.");
        const IniFile ini = IniFile::parse(text);
        if (ini.get("transmog", "version") != "2" || ini.entries("transmog").size() != 1u ||
            !ini.has_section("active"))
            throw psprecomp::Error("Unsupported transmog configuration in " + to_utf8(path) +
                "; expected [transmog] version=2 and an [active] section.");
        active = read_set(ini, "active");
        for (const auto &section : ini.sections()) {
            if (iequals(section, "transmog") || iequals(section, "active")) continue;
            if (!section.starts_with("preset:"))
                throw psprecomp::Error("Unknown transmog section [" + section + "] in " + to_utf8(path));
            const std::string name = ini.get(section, "name");
            validate_name(name);
            if (!presets.emplace(name, read_set(ini, section)).second)
                throw psprecomp::Error("Duplicate transmog preset name '" + name + "' in " + to_utf8(path));
        }
    }
    active_ = active;
    presets_ = std::move(presets);
    load_valid_ = true;
}

std::filesystem::path Store::back_up_invalid_and_reset() {
    if (path_.empty()) throw psprecomp::Error("Cannot recover transmog settings before the storage path is initialized.");
    if (load_valid_) throw psprecomp::Error("Transmog settings are valid; use Reset all to default to clear active appearances while retaining saved sets.");

    std::filesystem::path backup;
    for (std::uint32_t suffix = 0; suffix < 10000u; ++suffix) {
        backup = std::filesystem::path(path_.native() + std::filesystem::path(".invalid-backup-" + std::to_string(suffix)).native());
        std::error_code ec;
        const bool exists = std::filesystem::exists(backup, ec);
        if (ec) throw psprecomp::Error("Cannot inspect transmog backup path " + to_utf8(backup) + ": " + ec.message());
        if (!exists) break;
        backup.clear();
    }
    if (backup.empty()) throw psprecomp::Error("Cannot find an unused backup name for " + to_utf8(path_) + "; move an existing .invalid-backup-* file and retry.");

    std::error_code ec;
    const bool has_original = std::filesystem::exists(path_, ec);
    if (ec) throw psprecomp::Error("Cannot inspect invalid transmog settings " + to_utf8(path_) + ": " + ec.message());
    if (has_original) {
        std::filesystem::rename(path_, backup, ec);
        if (ec) throw psprecomp::Error("Cannot preserve invalid transmog settings " + to_utf8(path_) + " as " + to_utf8(backup) + ": " + ec.message());
    } else {
        backup.clear();
    }

    active_ = {};
    presets_.clear();
    load_valid_ = true;
    try {
        commit({}, {});
    } catch (...) {
        load_valid_ = false;
        throw;
    }
    return backup;
}

void Store::commit(Set active, Presets presets) {
    validate_set(active);
    if (path_.empty()) throw psprecomp::Error("Transmog storage has not been initialized; initialize it before editing appearances.");
    if (!load_valid_) throw psprecomp::Error("Transmog settings are invalid and have not been recovered; back up the file before saving changes.");
    if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path());
    const auto temporary = std::filesystem::path(path_.native() + std::filesystem::path(".part").native());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << "[transmog]\nversion=2\n\n[active]\n";
        write_set(output, active);
        unsigned index{};
        for (const auto &[name, set] : presets) {
            validate_name(name);
            validate_set(set);
            output << "\n[preset:" << index++ << "]\nname=\"" << name << "\"\n";
            write_set(output, set);
        }
        output.close();
        if (!output) throw psprecomp::Error("Cannot write transmog file " + to_utf8(temporary) + "; check directory permissions and free disk space.");
    }
    std::filesystem::rename(temporary, path_);
    active_ = active;
    presets_ = std::move(presets);
}

void Store::set(std::size_t slot, std::optional<ArmorAppearance> armor) {
    if (slot >= active_.armor.size()) throw psprecomp::Error("Invalid transmog slot " + std::to_string(slot) + "; expected 0 to 4.");
    Set next = active_;
    next.armor[slot] = armor;
    commit(next, presets_);
}

void Store::set_weapon(std::optional<WeaponAppearance> weapon) {
    Set next = active_;
    next.weapon = weapon;
    commit(next, presets_);
}

void Store::apply(Set active) { commit(std::move(active), presets_); }

void Store::reset() { commit({}, presets_); }

void Store::save_preset(std::string_view name) {
    validate_name(name);
    Presets next = presets_;
    next.insert_or_assign(std::string(name), active_);
    commit(active_, std::move(next));
}

void Store::save_preset(std::string_view name, Set set) {
    validate_name(name);
    validate_set(set);
    Presets next = presets_;
    next.insert_or_assign(std::string(name), std::move(set));
    commit(active_, std::move(next));
}

void Store::load_preset(std::string_view name) {
    const auto found = presets_.find(std::string(name));
    if (found == presets_.end()) throw psprecomp::Error("Cannot load missing transmog preset '" + std::string(name) + "'.");
    commit(found->second, presets_);
}

void Store::delete_preset(std::string_view name) {
    Presets next = presets_;
    if (next.erase(std::string(name)) != 1u)
        throw psprecomp::Error("Cannot delete missing transmog preset '" + std::string(name) + "'.");
    commit(active_, std::move(next));
}

} // namespace mhp3rd::mods::transmog
