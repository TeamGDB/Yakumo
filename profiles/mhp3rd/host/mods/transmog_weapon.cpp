#include "mods/transmog_weapon.hpp"

#include "game/guest_ram.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace mhp3rd::mods::transmog::weapon {
namespace {
constexpr std::uint32_t kHelper = 0x0886A568u;
constexpr std::uint32_t kSharedVisualLookup = 0x0886A47Cu;
constexpr std::uint32_t kLocalVisualEntry = 0x0886A534u;
constexpr std::uint32_t kCaller = 0x088F4738u;
constexpr std::uint32_t kLocalCaller = 0x088A59C8u;
constexpr std::uint32_t kHunterRegistry = 0x09FBE794u;
constexpr std::uint32_t kLocalHunterVtable = 0x0896FBC8u;
constexpr std::uint32_t kRemoteHunterVtable = 0x0896FED8u;
constexpr std::uint32_t kWeaponRecords = 0x08AB3640u;
constexpr std::uint32_t kHunterObjectIndex = 0x60u;
constexpr std::uint32_t kHunterFileFunctionSlot = 0xA8u;
constexpr std::uint32_t kOwnHunterRecord = 0u;
constexpr std::uint32_t kHunterFileFunction = 0x088A58DCu;
constexpr std::string_view kExecutable = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";
// NPJB-40001's 0x0885D374 dispatcher returns these records; 0x0886A47C
// reads only their first halfword for the weapon's visual model index.
constexpr ModelTable kTables[]{
    {5, 5, 0x08997AA0u, 0x1Cu}, {12, 17, 0x08997138u, 0x1Cu},
    {6, 7, 0x089953B0u, 0x1Cu}, {16, 25, 0x08994A9Cu, 0x1Cu},
    {7, 9, 0x08994054u, 0x1Cu}, {17, 27, 0x089936ECu, 0x1Cu},
    {8, 11, 0x0899669Cu, 0x1Cu}, {14, 21, 0x08995E14u, 0x1Cu},
    {13, 19, 0x08992F0Cu, 0x1Cu}, {11, 15, 0x08991954u, 0x50u},
    {9, 13, 0x08990464u, 0x50u}, {15, 23, 0x0898EB14u, 0x50u},
};
psprecomp::Runtime *installed_runtime{};
AppearanceFunction original{};
Store *settings{};
bool (*enabled)(){};
CatalogFunctions catalog{};
SelectionFunction visual_selection{};
std::string problem;
std::optional<WeaponAppearance> failed_choice;

const ModelTable &table(std::uint8_t kind) {
    for (const auto &entry : kTables) if (entry.kind == kind) return entry;
    throw psprecomp::Error("[transmog] Unsupported weapon equipment kind " + std::to_string(kind) + "; choose a supported weapon class.");
}

bool is_own_hunter(const psprecomp::GuestMemory &memory, std::uint32_t actor) {
    // Match the established layered_armor::own_hunter check: the instance's
    // record index alone is not enough to distinguish the player from another
    // registered hunter.
    if (memory.load16(actor + kHunterObjectIndex) != kOwnHunterRecord) return false;
    const auto vtable = memory.load32(actor);
    if (!memory.contains(vtable + kHunterFileFunctionSlot, 4u)) return false;
    return memory.load32(vtable + kHunterFileFunctionSlot) == kHunterFileFunction;
}
class ReadOnlyRam final : public game::Ram {
public:
    explicit ReadOnlyRam(const psprecomp::GuestMemory &memory) : memory_(memory) {}
    bool contains(std::uint32_t address, std::size_t length) const override { return memory_.contains(address, length); }
    std::uint8_t load8(std::uint32_t address) const override { return memory_.load8(address); }
    std::uint16_t load16(std::uint32_t address) const override { return memory_.load16(address); }
    std::uint32_t load32(std::uint32_t address) const override { return memory_.load32(address); }
    void store8(std::uint32_t, std::uint8_t) override { write_error(); }
    void store16(std::uint32_t, std::uint16_t) override { write_error(); }
    void store32(std::uint32_t, std::uint32_t) override { write_error(); }
private:
    [[noreturn]] static void write_error() { throw psprecomp::Error("[transmog] Weapon appearance lookup attempted a forbidden guest memory write."); }
    const psprecomp::GuestMemory &memory_;
};

void appearance(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto caller = ctx.gpr[31];
    const auto record = ctx.gpr[5];
    std::optional<std::uint8_t> kind;
    if (caller == kCaller) {
        if (!runtime.memory().contains(record, 4u))
            throw psprecomp::Error("[transmog] Weapon model loader supplied an equipment record outside guest RAM: " + psprecomp::hex32(record));
        kind = runtime.memory().load8(record + 1u);
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
            using LookupTrace = std::array<std::uint32_t, 4>;
            static LookupTrace previous{};
            const auto &choice = visual_selection().weapon;
            const LookupTrace observed{record, *kind, runtime.memory().load16(record + 2u), choice ? choice->id : 0u};
            if (observed != previous) {
                previous = observed;
                std::cout << "[transmog] Native weapon visual record=" << psprecomp::hex32(record)
                          << " class=" << unsigned(observed[1]) << " item=" << observed[2]
                          << " donor=" << observed[3] << '\n' << std::flush;
            }
        }
    }
    original(runtime, ctx);
    if (caller != kCaller || !enabled()) return;
    const auto choice = visual_selection().weapon;
    if (!choice) return;
    if (ctx.pc != caller)
        throw psprecomp::Error("[transmog] Original weapon appearance getter did not return to " + psprecomp::hex32(caller) + "; disable layered armor and restore the supported executable.");
    if (choice->kind != *kind) { problem.clear(); failed_choice.reset(); return; }
    try {
        ReadOnlyRam ram(runtime.memory());
        const auto selected = select_appearance(ram, choice, catalog.count(choice->kind), *kind);
        if (selected) {
            ctx.gpr[2] = *selected;
            if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
                using OverrideTrace = std::array<std::uint32_t, 3>;
                static OverrideTrace previous{};
                const OverrideTrace observed{*kind, choice->id, *selected};
                if (observed != previous) {
                    previous = observed;
                    std::cout << "[transmog] Weapon donor override class=" << unsigned(*kind)
                              << " item=" << choice->id << " model=" << *selected << '\n' << std::flush;
                }
            }
        }
        problem.clear();
        failed_choice.reset();
    } catch (const psprecomp::Error &error) {
        const std::string message = std::string(error.what()) + " The equipped weapon's original appearance remains active; armor choices are unchanged.";
        if (problem != message) std::cerr << message << '\n';
        problem = message;
        failed_choice = choice;
    }
}

// The local hunter resource builder enters at 0x0886A534, which decodes its
// native record then branches to 0x0886A47C inside the same generated unit.
// Hook the entry as well as the shared leaf so that intra-unit branch cannot
// bypass the donor selection. Scope both hooks to the local visual return site.
void local_visual_lookup(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto entry = ctx.pc;
    const auto caller = ctx.gpr[31];
    original(runtime, ctx);
    if (caller != kLocalCaller || !enabled()) return;
    if (ctx.pc != caller)
        throw psprecomp::Error("[transmog] Local weapon model lookup did not return to its verified visual caller.");
    const auto kind = static_cast<std::uint8_t>(ctx.gpr[5]);
    if (entry == kLocalVisualEntry && std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
        using EntryTrace = std::array<std::uint32_t, 3>;
        static EntryTrace previous{};
        const EntryTrace observed{kind, ctx.gpr[6], ctx.gpr[2]};
        if (observed != previous) {
            previous = observed;
            std::cout << "[transmog] Local weapon visual entry=" << psprecomp::hex32(entry)
                      << " class=" << unsigned(kind) << " item=" << observed[1]
                      << " native-model=" << observed[2] << '\n' << std::flush;
        }
    }
    const auto choice = visual_selection().weapon;
    if (!choice || choice->kind != kind) { problem.clear(); failed_choice.reset(); return; }
    try {
        ReadOnlyRam ram(runtime.memory());
        const auto selected = select_appearance(ram, choice, catalog.count(choice->kind), kind);
        if (selected) {
            ctx.gpr[2] = *selected;
            if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
                using OverrideTrace = std::array<std::uint32_t, 3>;
                static OverrideTrace previous{};
                const OverrideTrace observed{kind, choice->id, *selected};
                if (observed != previous) {
                    previous = observed;
                    std::cout << "[transmog] Weapon donor override class=" << unsigned(kind)
                              << " item=" << choice->id << " model=" << *selected << '\n' << std::flush;
                }
            }
        }
        problem.clear();
        failed_choice.reset();
    } catch (const psprecomp::Error &error) {
        const std::string message = std::string(error.what()) + " The equipped weapon's original appearance remains active; armor choices are unchanged.";
        if (problem != message) std::cerr << message << '\n';
        problem = message;
        failed_choice = choice;
    }
}
}

std::span<const ModelTable> model_tables() { return kTables; }

std::optional<std::uint16_t> select_appearance(
    const game::Ram &ram, std::optional<WeaponAppearance> selection,
    std::size_t count, std::uint8_t equipped_kind) {
    if (!selection) return std::nullopt;
    const auto &entry = table(selection->kind);
    if (selection->kind != equipped_kind) return std::nullopt;
    if (count == 0u || count > 4096u || selection->id == 0u || selection->id >= count)
        throw psprecomp::Error("[transmog] Weapon appearance id " + std::to_string(selection->id) +
            " is outside its loaded catalog (" + std::to_string(count) + " entries); choose a supported weapon appearance.");
    const auto address = entry.address + static_cast<std::uint32_t>(selection->id) * entry.stride;
    if (!ram.contains(address, 2u))
        throw psprecomp::Error("[transmog] Weapon model lookup is outside guest RAM; restore the supported executable.");
    return ram.load16(address);
}

void install(psprecomp::Runtime &runtime, AppearanceFunction function, Store &store,
             bool (*feature_enabled)(), CatalogFunctions catalog_readers, SelectionFunction selection_reader, std::string_view executable_sha256) {
    const char *disabled = std::getenv("MHP3RD_NO_TRANSMOG");
    if (disabled != nullptr && std::string_view(disabled) != "0" && std::string_view(disabled) != "1")
        throw psprecomp::Error("MHP3RD_NO_TRANSMOG must be 0 or 1; unset it to enable layered armor and weapons.");
    if (disabled != nullptr && std::string_view(disabled) == "1") return;
    if (executable_sha256 != kExecutable || function == nullptr || feature_enabled == nullptr ||
        catalog_readers.names == nullptr || catalog_readers.count == nullptr || selection_reader == nullptr ||
        !runtime.has_function(kHelper) || !runtime.has_function(kLocalVisualEntry) || installed_runtime == &runtime)
        throw psprecomp::Error("[transmog] Weapon appearance runtime requires the supported NPJB-40001 executable, original getter and catalog callbacks; install it once after generated functions.");
    constexpr CodeWord signature[]{
        {0x0886A568u, 0x94A60002u}, {0x0886A56Cu, 0x90A50001u}, {0x0886A570u, 0x0A21A91Fu},
        {0x088F4730u, 0x0E21A95Au}, {0x088F4734u, 0x00000000u},
        {0x0886A534u, 0x00051200u}, {0x0886A538u, 0x00052880u},
        {0x0886A53Cu, 0x00451023u}, {0x0886A550u, 0x0A21A91Fu},
        {0x088A59C0u, 0x0E21A94Du}, {0x088A59C4u, 0x00000000u},
    };
    for (const auto &word : signature)
        if (runtime.memory().load32(word.address) != word.word)
            throw psprecomp::Error("[transmog] Unsupported weapon visual code at " + psprecomp::hex32(word.address) + "; restore the supported executable or disable layered armor.");
    original = function;
    settings = &store;
    enabled = feature_enabled;
    catalog = catalog_readers;
    visual_selection = selection_reader;
    installed_runtime = &runtime;
    runtime.register_function(kHelper, &appearance, "mhp3rd_layered_weapon_model_index");
    runtime.register_function(kSharedVisualLookup, &local_visual_lookup, "mhp3rd_layered_local_weapon_model_index");
    runtime.register_function(kLocalVisualEntry, &local_visual_lookup, "mhp3rd_layered_local_weapon_record_index");
}

std::vector<Armor> weapon_list(std::uint8_t kind) {
    (void)table(kind);
    if (installed_runtime == nullptr) return {};
    const auto entries = catalog.names(kind);
    if (entries.size() > 4096u) throw psprecomp::Error("[transmog] Weapon name table exceeds the supported catalog bound.");
    std::vector<Armor> result;
    for (std::size_t id = 1u; id < entries.size(); ++id)
        if (!entries[id].empty()) result.push_back({static_cast<std::uint16_t>(id), entries[id]});
    return result;
}

std::optional<std::uint8_t> equipped_weapon_kind() {
    if (installed_runtime == nullptr) return std::nullopt;
    const auto &ram = installed_runtime->memory();
    if (!ram.contains(kHunterRegistry, 4u) || !ram.contains(kWeaponRecords, 4u))
        throw psprecomp::Error("[transmog] Native hunter registry or weapon-record pointer is outside guest RAM.");
    const auto registry = ram.load32(kHunterRegistry);
    const auto records = ram.load32(kWeaponRecords);
    if (registry == 0u || records == 0u) return std::nullopt;
    if (!ram.contains(registry, 32u) || !ram.contains(records, 4u))
        throw psprecomp::Error("[transmog] Native hunter registry or weapon-record table is outside guest RAM.");
    std::optional<std::uint8_t> kind;
    for (std::uint32_t slot = 0u; slot < 6u; ++slot) {
        const auto actor = ram.load32(registry + 8u + slot * 4u);
        if (actor == 0u) continue;
        if (!ram.contains(actor, 5300u))
            throw psprecomp::Error("[transmog] Registered hunter is outside guest RAM while reading its weapon class.");
        const auto vtable = ram.load32(actor);
        if ((vtable != kLocalHunterVtable && vtable != kRemoteHunterVtable) ||
            (ram.load32(actor + 4u) & 1u) == 0u ||
            ram.load8(actor + 5154u) == 0u || ram.load32(actor + 2944u) != 4u) continue;
        // Both actor layouts appear in the registry. Only the actor the game
        // draws from its own record-zero file function reports local status;
        // ready remote hunters still have valid weapon data.
        if (!is_own_hunter(ram, actor)) continue;
        if (kind)
            throw psprecomp::Error("[transmog] More than one ready local hunter is registered; weapon class is ambiguous.");
        std::uint32_t identity = 0xFFFFFFFFu;
        std::uint16_t id{};
        std::uint8_t native_kind{};
        if (vtable == kLocalHunterVtable) {
            identity = ram.load16(actor + 96u);
            if (identity >= 6u)
                throw psprecomp::Error("[transmog] Local hunter identity " + std::to_string(identity) +
                    " is outside the six-entry native registry; restore the supported executable.");
            const auto record = records + identity * 252u + 48u;
            if (!ram.contains(record, 120u))
                throw psprecomp::Error("[transmog] Native equipped weapon record for hunter " + std::to_string(identity) + " is outside guest RAM.");
            id = ram.load16(record + 78u);
            native_kind = ram.load8(record + 118u);
        } else {
            const auto record = actor + 1160u;
            if (!ram.contains(record, 4u))
                throw psprecomp::Error("[transmog] Native local-hunter weapon record is outside guest RAM.");
            id = ram.load16(record + 2u);
            native_kind = ram.load8(record + 1u);
        }
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
            using WeaponTrace = std::array<std::uint32_t, 4>;
            static WeaponTrace previous{};
            const WeaponTrace observed{actor, identity, id, native_kind};
            if (observed != previous) {
                previous = observed;
                std::cout << "[transmog] Native equipped weapon actor=" << psprecomp::hex32(actor)
                          << " vtable=" << psprecomp::hex32(vtable) << " identity=" << identity
                          << " item=" << id << " class=" << unsigned(native_kind) << '\n' << std::flush;
            }
        }
        if (id == 0u) return std::nullopt;
        (void)table(native_kind);
        kind = native_kind;
    }
    if (!kind) return std::nullopt;
    return kind;
}

std::uint16_t effective_model_for_actor(
    const game::Ram &ram, std::uint32_t actor, std::optional<WeaponAppearance> selection,
    std::size_t donor_count) {
    if (!ram.contains(actor, 5300u))
        throw psprecomp::Error("[transmog] Hunter actor is outside guest RAM while checking the cached weapon resource.");
    const auto vtable = ram.load32(actor);
    std::uint16_t weapon_id{};
    std::uint8_t native_kind{};
    if (vtable == kLocalHunterVtable) {
        const auto identity = ram.load16(actor + 96u);
        if (identity >= 6u)
            throw psprecomp::Error("[transmog] Local hunter identity " + std::to_string(identity) +
                " is outside the six-entry native registry while checking its weapon resource.");
        if (!ram.contains(kWeaponRecords, 4u))
            throw psprecomp::Error("[transmog] Native weapon-record table pointer is outside guest RAM.");
        const auto records = ram.load32(kWeaponRecords);
        const auto record = records + static_cast<std::uint32_t>(identity) * 252u + 48u;
        if (records == 0u || !ram.contains(record, 120u))
            throw psprecomp::Error("[transmog] Native local-hunter weapon record is outside guest RAM.");
        weapon_id = ram.load16(record + 78u);
        native_kind = ram.load8(record + 118u);
    } else if (vtable == kRemoteHunterVtable) {
        const auto record = actor + 1160u;
        if (!ram.contains(record, 4u))
            throw psprecomp::Error("[transmog] Native remote-hunter weapon record is outside guest RAM.");
        weapon_id = ram.load16(record + 2u);
        native_kind = ram.load8(record + 1u);
    } else {
        throw psprecomp::Error("[transmog] Hunter vtable " + psprecomp::hex32(vtable) +
            " has no verified weapon-resource layout.");
    }

    const auto &native_table = table(native_kind);
    const auto native_address = native_table.address + static_cast<std::uint32_t>(weapon_id) * native_table.stride;
    if (!ram.contains(native_address, 2u))
        throw psprecomp::Error("[transmog] Equipped weapon model is outside guest RAM for class " +
            std::to_string(native_kind) + " item " + std::to_string(weapon_id) + ".");
    auto model = ram.load16(native_address);
    if (selection && selection->kind == native_kind) {
        // Invalid saved donors are handled visibly by the live getter, which
        // keeps the native model. Use that same stock result when deciding
        // whether the already-installed slot-7 resource can stay in place.
        if (donor_count > 0u && donor_count <= 4096u && selection->id > 0u && selection->id < donor_count) {
            const auto selected = select_appearance(ram, selection, donor_count, native_kind);
            if (!selected)
                throw psprecomp::Error("[transmog] Matching weapon donor did not resolve to a model index.");
            model = *selected;
        }
    }
    return model;
}

std::uint16_t effective_resource_id_for_actor(
    const game::Ram &ram, std::uint32_t actor, std::optional<WeaponAppearance> selection,
    std::size_t donor_count) {
    // Both verified hunter resource getters clamp the weapon model index to
    // the category-specific model count, then add its archive resource base
    // before A5564 compares it with actor+2956+4*slot.
    constexpr std::uint32_t kWeaponModelCounts = 0x089CD1F8u;
    constexpr std::uint32_t kWeaponResourceBases = 0x089CD22Cu;
    constexpr std::uint32_t kResourceCategoryCount = 13u;
    if (!ram.contains(actor + 98u, 1u) ||
        !ram.contains(kWeaponModelCounts, kResourceCategoryCount * 4u) ||
        !ram.contains(kWeaponResourceBases, kResourceCategoryCount * 2u))
        throw psprecomp::Error("[transmog] Weapon resource mapping metadata is outside guest RAM.");
    const auto category = ram.load8(actor + 98u);
    if (category >= kResourceCategoryCount)
        throw psprecomp::Error("[transmog] Native weapon resource category " + std::to_string(category) +
            " is outside the 13 verified categories.");
    const auto count = ram.load16(kWeaponModelCounts + category * 4u);
    if (count == 0u)
        throw psprecomp::Error("[transmog] Native weapon model count is zero for resource category " + std::to_string(category) + ".");
    const auto base = ram.load16(kWeaponResourceBases + category * 2u);
    const auto model = effective_model_for_actor(ram, actor, selection, donor_count);
    const auto bounded_model = std::min<std::uint16_t>(model, static_cast<std::uint16_t>(count - 1u));
    return static_cast<std::uint16_t>(base + bounded_model);
}

std::string status() {
    if (visual_selection == nullptr || visual_selection().weapon != failed_choice) {
        problem.clear();
        failed_choice.reset();
    }
    return problem;
}

} // namespace mhp3rd::mods::transmog::weapon
