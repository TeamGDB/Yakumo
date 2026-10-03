#include "mods/transmog.hpp"
#include "mods/transmog_weapon.hpp"

#include "game/guest_ram.hpp"
#include "install/user_data.hpp"
#include "kernel/kernel.hpp"
#include "mods/mod_ini.hpp"
#include "settings/settings.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <memory>

namespace mhp3rd::mods::transmog {
namespace {
constexpr std::uint32_t kHelper = 0x08869564u;
constexpr std::uint32_t kLocalHelper = 0x08869778u;
constexpr std::uint32_t kConverter = 0x088A51E4u;
constexpr std::uint32_t kLocalResourceResolver = 0x088A58DCu;
constexpr std::uint32_t kLocalResourceCaller = 0x088A5624u;
constexpr std::string_view kExecutable = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";
constexpr std::array<std::uint32_t, 5> kTables{0x08987EE4u, 0x08985A7Cu, 0x08983934u, 0x0898A6E4u, 0x0898C854u};
constexpr std::array<std::uint8_t, 5> kKinds{4u, 0u, 1u, 2u, 3u};
constexpr std::array<std::uint32_t, 5> kNameTables{29u, 31u, 33u, 35u, 37u};
constexpr CodeWord kSignature[] = {
    {0x08869564u, 0x30C600FFu}, {0x08869568u, 0x2CC20007u},
    {0x0886956Cu, 0x30A500FFu}, {0x08869570u, 0x30E7FFFFu},
    {0x08869574u, 0x310800FFu}, {0x08869578u, 0x2403FFFFu},
    {0x088A51E4u, 0x30C6FFFFu}, {0x088A51E8u, 0x30A500FFu},
    {0x088A51ECu, 0x24020001u}, {0x088A51F0u, 0x30E900FFu},
    {0x088A51F4u, 0x308400FFu}, {0x088A51F8u, 0x310700FFu},
};
// Both callers build visual model indices/resource ids. The equipment
// availability helper 0x0886A108 and all gameplay stat readers stay original.
constexpr std::uint32_t kCallers[]{0x088F4270u, 0x088F47ECu};
psprecomp::Runtime *installed_runtime{};
AppearanceFunction original{};
AppearanceFunction original_converter{};
Store settings;
bool settings_initialized{};
bool legacy_migration_pending{};
bool runtime_disabled{};
std::optional<Set> preview_selection;
std::string installation_problem{"Layered armor runtime is not installed."};
std::string saved_appearance_problem;
std::string settings_load_problem;
bool refresh_pending{};
Set observed_selection;
std::string refresh_message;
struct RefreshJob;
std::shared_ptr<RefreshJob> current_refresh;
struct GeRefreshFence {
    std::int32_t thread{};
    std::uint32_t head{};
    std::uint32_t list_id{};
    std::uint64_t deadline_us{};
    bool matched{};
    bool done{};
    bool finish_queued{};
    bool finish_returned{};
    bool callback_dispatch_observed{};
};
std::optional<GeRefreshFence> ge_refresh_fence;
bool ge_refresh_ready{};

void post_dispatch_refresh(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx,
                           std::uint32_t dispatch_pc, std::int32_t dispatch_thread) {
    if (!ge_refresh_fence) return;
    auto &fence = *ge_refresh_fence;
    if (kernel().now_us() > fence.deadline_us)
        throw psprecomp::Error("[transmog] Pending visual refresh never reached a completed GE list and finish callback (list " +
            std::to_string(fence.list_id) + ", head " + psprecomp::hex32(fence.head) +
            ", done=" + std::to_string(fence.done) + ", callback=" + std::to_string(fence.finish_returned) +
            "). Reload the hunter before changing appearances.");
    const auto *owner = kernel().find_thread(fence.thread);
    if (owner == nullptr || owner->status == ThreadStatus::Dormant || owner->status == ThreadStatus::Dead)
        throw psprecomp::Error("[transmog] Visual refresh owner thread exited before its pending GE list completed.");
    if (!fence.done || !fence.finish_queued || !fence.finish_returned)
        return;
    // The finish callback can wake a higher-priority thread as its interrupt
    // returns. Remember that return, then wait until the owner is dispatched
    // again before calling any native visual refresh function.
    if (dispatch_pc == kInterruptReturnStub)
        fence.callback_dispatch_observed = true;
    if (!fence.callback_dispatch_observed || kernel().in_interrupt() || kernel().current_uid() != fence.thread ||
        (dispatch_pc != kInterruptReturnStub && dispatch_thread != fence.thread))
        return;
    if (dispatch_pc == kInterruptReturnStub && ctx.pc != ctx.gpr[31])
        throw psprecomp::Error("[transmog] GE callback returned to an unexpected guest continuation " +
            psprecomp::hex32(ctx.pc) + " (ra " + psprecomp::hex32(ctx.gpr[31]) + ").");
    if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
        std::cout << "[transmog] GE fence passed list=" << fence.list_id << " continuation="
                  << psprecomp::hex32(ctx.pc) << " v0=" << psprecomp::hex32(ctx.gpr[2]) << '\n' << std::flush;
    ge_refresh_fence.reset();
    ge_refresh_ready = true;
    struct ResetReady {
        ~ResetReady() { ge_refresh_ready = false; }
    } reset_ready;
    frame(runtime, ctx);
}

// The production feature has no dependency on developer cheats. The Ram
// interface is shared with synthetic tests; this adapter refuses all writes.
class ReadOnlyRam final : public game::Ram {
public:
    explicit ReadOnlyRam(const psprecomp::GuestMemory &memory) : memory_(memory) {}
    bool contains(std::uint32_t address, std::size_t length) const override { return memory_.contains(address, length); }
    std::uint8_t load8(std::uint32_t address) const override { return memory_.load8(address); }
    std::uint16_t load16(std::uint32_t address) const override { return memory_.load16(address); }
    std::uint32_t load32(std::uint32_t address) const override { return memory_.load32(address); }
    void store8(std::uint32_t, std::uint8_t) override { throw psprecomp::Error("[transmog] Unexpected write through read-only appearance memory."); }
    void store16(std::uint32_t, std::uint16_t) override { throw psprecomp::Error("[transmog] Unexpected write through read-only appearance memory."); }
    void store32(std::uint32_t, std::uint32_t) override { throw psprecomp::Error("[transmog] Unexpected write through read-only appearance memory."); }
private:
    const psprecomp::GuestMemory &memory_;
};

bool character_loaded(const game::Ram &ram) {
    constexpr std::uint32_t kHunterName = 0x09F4FCACu;
    return ram.contains(kHunterName, 2u) && ram.load16(kHunterName) != 0u;
}

struct NameDirectory { std::uint32_t table{}; std::uint32_t count{}; };
NameDirectory name_directory(const game::Ram &ram, std::uint32_t index) {
    constexpr std::uint32_t kText = 0x08A40640u;
    const auto entry = kText + index * 4u;
    if (!ram.contains(entry, 4u)) throw psprecomp::Error("[transmog] Armor name header is outside guest RAM.");
    const auto offset = ram.load32(entry);
    // Zero is the game's unloaded text block, before startup finishes.
    if (offset == 0u) return {};
    if (offset > 0x100000u) throw psprecomp::Error("[transmog] Unsupported armor name-table offset; restore the supported game text.");
    const auto table = kText + offset;
    if (!ram.contains(table, 8u)) throw psprecomp::Error("[transmog] Armor name table is outside guest RAM.");
    const auto first = ram.load32(table);
    if (first < 8u || first % 4u != 0u || first / 4u - 1u > 4096u || !ram.contains(table, first))
        throw psprecomp::Error("[transmog] Unsupported armor name-table directory; restore the supported game text.");
    const auto count = first / 4u - 1u;
    if (ram.load32(table + count * 4u) != 0xFFFFFFFFu)
        throw psprecomp::Error("[transmog] Armor name-table directory has no end marker; restore the supported game text.");
    return {table, count};
}

std::vector<std::string> names_table(const game::Ram &ram, std::uint32_t index) {
    const auto [table, count] = name_directory(ram, index);
    std::vector<std::string> names;
    names.reserve(count);
    for (std::uint32_t id = 0; id < count; ++id) {
        const auto name_offset = ram.load32(table + id * 4u);
        if (name_offset > 0x100000u) throw psprecomp::Error("[transmog] Armor name offset exceeds the supported text block.");
        const auto address = table + name_offset;
        std::string name;
        bool terminated = false;
        for (std::uint32_t length = 0; length < 256u; ++length) {
            if (!ram.contains(address + length, 1u)) throw psprecomp::Error("[transmog] Armor name extends outside guest RAM.");
            const auto value = ram.load8(address + length);
            if (value == 0u) { terminated = true; break; }
            name.push_back(static_cast<char>(value));
        }
        if (!terminated) throw psprecomp::Error("[transmog] Armor name is longer than the supported 255 bytes.");
        names.push_back(std::move(name));
    }
    return names;
}

std::vector<std::string> armor_names(const game::Ram &ram, std::size_t slot) {
    return names_table(ram, kNameTables[slot]);
}

void ensure_settings() {
    if (!runtime_disabled && !settings_initialized) {
        const auto path = install::user_data_directory() / "transmog.ini";
        std::error_code inspect_error;
        const bool existing = std::filesystem::exists(path, inspect_error);
        if (inspect_error)
            throw psprecomp::Error("[transmog] Cannot inspect appearance store " + ::mhp3rd::mods::to_utf8(path) +
                ": " + inspect_error.message());
        try {
            settings.initialize(path);
        } catch (const psprecomp::Error &error) {
            settings_initialized = true;
            settings_load_problem = "Cannot load saved transmog settings at " + ::mhp3rd::mods::to_utf8(path) + ": " + error.what() +
                " Original equipment appearances remain active. Open Transmog to back up the file and restore defaults.";
            std::cerr << "[transmog] " << settings_load_problem << '\n';
            return;
        }
        settings_initialized = true;
        if (!existing)
            legacy_migration_pending = std::any_of(settings::current().layered_pieces.begin(),
                settings::current().layered_pieces.end(), [](std::int32_t id) { return id >= 0; });
    }
}

std::uint32_t character_gender(const game::Ram &ram);
std::array<std::size_t, 5> catalog_counts(const game::Ram &ram);
void validate_set(const Set &selection);

void migrate_legacy_if_ready() {
    if (!legacy_migration_pending || installed_runtime == nullptr || !settings_load_problem.empty()) return;
    ReadOnlyRam ram(installed_runtime->memory());
    if (!character_loaded(ram)) return;
    const auto counts = catalog_counts(ram);
    if (std::any_of(counts.begin(), counts.end(), [](std::size_t count) { return count == 0u; })) return;
    const auto gender = static_cast<std::uint8_t>(character_gender(ram));
    const auto &legacy = settings::current().layered_pieces;
    Set migrated;
    for (std::size_t part = 0; part < legacy.size(); ++part)
        if (legacy[part] >= 0)
            migrated.armor[part == 4u ? 0u : part + 1u] =
                ArmorAppearance{static_cast<std::uint16_t>(legacy[part]), gender};
    validate_set(migrated);
    settings.apply(migrated);
    legacy_migration_pending = false;
    std::cout << "[transmog] Imported legacy layered armor choices into transmog.ini.\n";
}

std::array<std::size_t, 5> catalog_counts(const game::Ram &ram) {
    std::array<std::size_t, 5> counts{};
    for (std::size_t slot = 0; slot < counts.size(); ++slot)
        counts[slot] = name_directory(ram, kNameTables[slot]).count;
    return counts;
}

std::uint32_t character_gender(const game::Ram &ram) {
    constexpr std::uint32_t kCharacterPointer = 0x08AB3640u;
    if (!ram.contains(kCharacterPointer, 4u)) throw psprecomp::Error("[transmog] Character pointer is outside guest RAM.");
    const auto character = ram.load32(kCharacterPointer);
    if (!ram.contains(character + 0x877u, 1u))
        throw psprecomp::Error("[transmog] Loaded character gender is outside guest RAM; restore the supported executable.");
    const auto gender = ram.load8(character + 0x877u);
    if (gender > 1u) throw psprecomp::Error("[transmog] Unsupported character gender " + std::to_string(gender) + "; restore the supported save/executable.");
    return gender;
}

void appearance(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto caller = ctx.gpr[31];
    const auto kind = ctx.gpr[6] & 0xFFu;
    const auto gender = ctx.gpr[5] & 0xFFu;
    original(runtime, ctx);
    if (kind >= 5u || !enabled() ||
        std::find(std::begin(kCallers), std::end(kCallers), caller) == std::end(kCallers)) return;
    ensure_settings();
    if (ctx.pc != caller)
        throw psprecomp::Error("[transmog] Original appearance getter did not return to " + psprecomp::hex32(caller) + "; disable layered armor and restore the supported executable.");
    ReadOnlyRam ram(runtime.memory());
    // A persisted set can reference a missing donor or unsupported catalog.
    // Keep the original result, report the specific failure once, and leave
    // Reset available. Errors in the original game helper still propagate.
    try {
        const auto counts = catalog_counts(ram);
        for (const auto armor_kind : kKinds) (void)select_appearance(ram, visual_selection(), counts, armor_kind, gender);
        const auto selected = select_appearance(ram, visual_selection(), counts, kind, gender);
        if (selected) ctx.gpr[2] = *selected;
    } catch (const psprecomp::Error &error) {
        if (current_refresh) throw;
        saved_appearance_problem = std::string(error.what()) + " Original appearances are active until this set is reset or corrected.";
        std::cerr << saved_appearance_problem << '\n';
    }
}

void convert_appearance(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto input = ctx;
    const auto kind = ctx.gpr[5];
    const auto caller = ctx.gpr[31];
    const bool local = caller == 0x088A5964u;
    original_converter(runtime, ctx);
    if (kind >= 5u || (caller != 0x088F482Cu && !local) || !enabled()) return;
    if (ctx.pc != caller)
        throw psprecomp::Error("[transmog] Original visual model converter did not return to its verified caller.");
    if (local && (input.gpr[16] == 0u || !runtime.memory().contains(input.gpr[16], 2u)))
        throw psprecomp::Error("[transmog] Local hunter converter received an invalid base-model pointer.");
    ReadOnlyRam ram(runtime.memory());
    const auto counts = catalog_counts(ram);
    for (const auto armor_kind : kKinds)
        (void)select_appearance(ram, visual_selection(), counts, armor_kind, 0u);
    const auto model = select_appearance(ram, visual_selection(), counts, kind, 0u);
    if (!model) return;
    const auto slot = kind == 4u ? 0u : kind + 1u;
    const auto donor = *visual_selection().armor[slot];
    // Both sex tables contain four native bare-body variations. A donor with
    // model zero reaches these tables even when the equipped armor did not.
    if (*model == 0u && (kind == 0u || kind == 1u || kind == 3u) && (input.gpr[8] & 0xFFu) > 3u)
        throw psprecomp::Error("[transmog] Bare armor received unsupported body variation " + std::to_string(input.gpr[8] & 0xFFu) + ".");
    auto conversion = input;
    conversion.pc = kConverter;
    conversion.gpr[4] = donor.gender;
    conversion.gpr[5] = kind;
    conversion.gpr[6] = *model;
    conversion.gpr[7] = 0u;
    // Preserve the actor's existing body variation argument.
    conversion.gpr[8] = input.gpr[8];
    conversion.gpr[31] = 0u;
    // This verified leaf only reads native default-model tables. Its isolated
    // context cannot change the hunter's gender, equipment or live registers.
    original_converter(runtime, conversion);
    if (conversion.pc != 0u)
        throw psprecomp::Error("[transmog] Donor model conversion did not complete; restore the supported executable.");
    const auto base = ram.load16(0x089E892Cu + (donor.gender * 7u + kind) * 2u);
    // Native F482C adds s0 (the actor-gender base), then masks to 16 bits.
    // Correct that one visual result while keeping its normal continuation.
    const auto caller_base = local ? ram.load16(input.gpr[16]) : static_cast<std::uint16_t>(input.gpr[16]);
    ctx.gpr[2] = converter_adjustment(base, static_cast<std::uint16_t>(conversion.gpr[2]), caller_base);
}

// FBC8 hunters use the local resource resolver at 0x088A58DC. Its getter
// call returns at 0x088A592C and shares the same verified AOT unit as the
// existing helper, but passes hunter id instead of gender in a1.
void local_appearance(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto caller = ctx.gpr[31];
    const auto kind = ctx.gpr[6] & 0xFFu;
    original(runtime, ctx);
    if ((caller != 0x088A592Cu && caller != 0x088A5B68u) || kind >= 5u || !enabled()) return;
    if (ctx.pc != caller)
        throw psprecomp::Error("[transmog] Local hunter armor getter did not return to its verified visual caller.");
    ReadOnlyRam ram(runtime.memory());
    const auto counts = catalog_counts(ram);
    for (const auto armor_kind : kKinds)
        (void)select_appearance(ram, visual_selection(), counts, armor_kind, 0u);
    const auto selected = select_appearance(ram, visual_selection(), counts, kind, 0u);
    if (selected) ctx.gpr[2] = *selected;
}

}

std::span<const CodeWord> helper_signature() { return kSignature; }
std::span<const std::uint32_t> callers() { return kCallers; }
std::uint16_t converter_adjustment(std::uint16_t donor_base, std::uint16_t converted_model,
    std::uint16_t caller_base) {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(donor_base) + converted_model - caller_base);
}
Store &store() { ensure_settings(); migrate_legacy_if_ready(); return settings; }

namespace {
void validate_set(const Set &selection) {
    ensure_settings();
    if (installed_runtime == nullptr)
        throw psprecomp::Error("[transmog] Cannot choose an appearance before the supported runtime is installed.");
    ReadOnlyRam ram(installed_runtime->memory());
    if (!character_loaded(ram))
        throw psprecomp::Error("[transmog] Load a hunter before choosing an appearance or loading a saved set.");
    const auto counts = catalog_counts(ram);
    const auto gender = character_gender(ram);
    for (const auto kind : kKinds) (void)select_appearance(ram, selection, counts, kind, gender);
    if (selection.weapon)
        (void)weapon::select_appearance(ram, selection.weapon,
            equipment_count(selection.weapon->kind), selection.weapon->kind);
}
}

void set_appearance(std::size_t slot, std::optional<ArmorAppearance> armor) {
    ensure_settings();
    if (slot >= kSlots.size()) throw psprecomp::Error("[transmog] Cannot edit invalid armor slot " + std::to_string(slot) + ".");
    if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
        std::cout << "[transmog] Appearance selection slot=" << slot;
        if (armor) std::cout << " id=" << armor->id << " gender=" << static_cast<unsigned>(armor->gender);
        else std::cout << " default";
        std::cout << '\n' << std::flush;
    }
    auto selection = settings.active();
    if (preview_selection) selection = *preview_selection;
    selection.armor[slot] = armor;
    validate_set(selection);
    if (preview_selection) preview_selection = selection;
    else settings.set(slot, armor);
    saved_appearance_problem.clear();
}

void load_set(std::string_view name) {
    ensure_settings();
    const auto found = settings.presets().find(std::string(name));
    if (found == settings.presets().end()) throw psprecomp::Error("[transmog] Saved set '" + std::string(name) + "' does not exist.");
    validate_set(found->second);
    const bool previewing = preview_selection.has_value();
    settings.load_preset(name);
    if (previewing) preview_selection = settings.active();
    saved_appearance_problem.clear();
}

void set_weapon_appearance(std::optional<WeaponAppearance> appearance) {
    ensure_settings();
    auto selection = settings.active();
    if (preview_selection) selection = *preview_selection;
    selection.weapon = appearance;
    validate_set(selection);
    if (preview_selection) preview_selection = selection;
    else settings.set_weapon(appearance);
    saved_appearance_problem.clear();
}

void reset_appearances() {
    ensure_settings();
    if (settings.path().empty()) throw psprecomp::Error("[transmog] The appearance store is disabled; unset MHP3RD_NO_TRANSMOG before resetting saved appearances.");
    if (!settings_load_problem.empty())
        throw psprecomp::Error("[transmog] Saved settings are invalid; use the explicit backup and recovery action instead of overwriting the file.");
    if (preview_selection) preview_selection = Set{};
    else settings.reset();
    saved_appearance_problem.clear();
}

std::string configuration_problem() {
    ensure_settings();
    return settings_load_problem;
}

std::filesystem::path back_up_invalid_settings_and_reset() {
    ensure_settings();
    if (settings_load_problem.empty())
        throw psprecomp::Error("[transmog] There is no invalid saved-settings file to recover.");
    const auto backup = settings.back_up_invalid_and_reset();
    settings_load_problem.clear();
    return backup;
}

bool preview_active() { return preview_selection.has_value(); }

const Set &current_selection() { migrate_legacy_if_ready(); return preview_selection ? *preview_selection : settings.active(); }

void begin_preview() {
    ensure_settings();
    if (!settings_load_problem.empty())
        throw psprecomp::Error("[transmog] Cannot preview until the invalid settings file is backed up and recovered.");
    if (!preview_selection) preview_selection = settings.active();
}

void apply_preview() {
    ensure_settings();
    if (!preview_selection) throw psprecomp::Error("[transmog] No preview selection is active to apply.");
    // The empty set has no donor ids to validate and is always safe to persist.
    if (*preview_selection != Set{}) validate_set(*preview_selection);
    settings.apply(*preview_selection);
    preview_selection.reset();
}

void cancel_preview() { preview_selection.reset(); }

void save_current_selection(std::string_view name) {
    ensure_settings();
    if (!settings_load_problem.empty())
        throw psprecomp::Error("[transmog] Cannot save a set until the invalid settings file is backed up and recovered.");
    settings.save_preset(name, current_selection());
}

namespace {
constexpr std::uint32_t kHunterRegistry = 0x09FBE794u;
constexpr std::uint32_t kVisualPool = 0x09FBE75Cu;
constexpr std::uint32_t kHunterVtable = 0x0896FED8u;
constexpr std::uint32_t kLocalHunterVtable = 0x0896FBC8u;
constexpr std::array<std::uint32_t, 6> kRefreshFunctions{
    0x088A53C8u, 0x088A3138u, 0x088A3474u,
    0x088F4204u, 0x088A5564u, 0x088A5A80u};
constexpr std::uint32_t kInstallArmorComponent = 0x088B1B64u;
constexpr std::uint32_t kFinalizeArmorComponents = 0x088A58CCu;
constexpr std::uint32_t kLocalCacheModelIndices = 0x088A5B20u;
constexpr std::uint32_t kRefreshPoseState = 0x088A1310u;

bool registered_local_hunter(const psprecomp::GuestMemory &memory, std::uint32_t actor) {
    if (!memory.contains(actor, 5300u) || memory.load32(actor) != kLocalHunterVtable) return false;
    if (!memory.contains(kHunterRegistry, 4u))
        throw psprecomp::Error("[transmog] Hunter registry pointer is outside guest RAM while resolving a local appearance.");
    const auto registry = memory.load32(kHunterRegistry);
    if (registry == 0u) return false;
    if (!memory.contains(registry, 32u))
        throw psprecomp::Error("[transmog] Hunter registry is invalid while resolving a local appearance.");
    for (std::uint32_t slot = 0u; slot < 6u; ++slot)
        if (memory.load32(registry + 8u + slot * 4u) == actor) return true;
    return false;
}

void local_resource_resolver(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    const auto caller = ctx.gpr[31];
    const auto actor = ctx.gpr[4];
    const auto kind = ctx.gpr[5] & 0xFFu;
    if (ctx.pc != kLocalResourceResolver)
        throw psprecomp::Error("[transmog] Local resource resolver received an unexpected entry PC " + psprecomp::hex32(ctx.pc) + ".");

    if (caller != kLocalResourceCaller || kind >= 5u || !enabled() ||
        !registered_local_hunter(runtime.memory(), actor)) {
        // Let the original AOT unit run through the normal dispatcher. Its
        // nested A69778 call is intercepted by local_appearance, then the
        // same-unit converter continues inside generated unit 0040.
        original_converter(runtime, ctx);
        return;
    }

    const auto &selection = visual_selection();
    const auto slot = kind == 4u ? 0u : kind + 1u;
    if (!selection.armor[slot]) {
        original_converter(runtime, ctx);
        return;
    }
    const auto donor = *selection.armor[slot];
    const auto &memory = runtime.memory();
    if (!memory.contains(actor + 1112u, 1u) || !memory.contains(actor + 1264u, 1u))
        throw psprecomp::Error("[transmog] Local hunter gender/body state is outside guest RAM during appearance resolution.");
    const auto actor_gender = memory.load8(actor + 1112u);
    if (actor_gender > 1u)
        throw psprecomp::Error("[transmog] Local hunter has unsupported resource gender " + std::to_string(actor_gender) + ".");
    if (donor.gender == actor_gender) {
        original_converter(runtime, ctx);
        return;
    }

    ReadOnlyRam ram(memory);
    const auto counts = catalog_counts(ram);
    const auto model = select_appearance(ram, selection, counts, kind, donor.gender);
    if (!model)
        throw psprecomp::Error("[transmog] Selected local armor donor disappeared during resource resolution.");
    const auto body_variation = memory.load8(actor + 1264u);
    if (*model == 0u && (kind == 0u || kind == 1u || kind == 3u) && body_variation > 3u)
        throw psprecomp::Error("[transmog] Bare armor received unsupported body variation " + std::to_string(body_variation) + ".");

    auto conversion = ctx;
    conversion.pc = kConverter;
    conversion.gpr[4] = donor.gender;
    conversion.gpr[5] = kind;
    conversion.gpr[6] = *model;
    conversion.gpr[7] = 0u;
    conversion.gpr[8] = body_variation;
    conversion.gpr[31] = 0u;
    original_converter(runtime, conversion);
    if (conversion.pc != 0u)
        throw psprecomp::Error("[transmog] Cross-sex local armor conversion did not return to its zero sentinel.");

    const auto base_address = 0x089E892Cu + (static_cast<std::uint32_t>(donor.gender) * 7u + kind) * 2u;
    if (!ram.contains(base_address, 2u))
        throw psprecomp::Error("[transmog] Donor resource base is outside guest RAM for local armor conversion.");
    const auto donor_base = ram.load16(base_address);
    const auto converted_model = static_cast<std::uint16_t>(conversion.gpr[2]);
    const auto desired_resource = static_cast<std::uint16_t>(static_cast<std::uint32_t>(donor_base) + converted_model);
    if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
        constexpr std::uint32_t kTraceLimit = 64u;
        static std::uint32_t trace_count{};
        if (trace_count++ < kTraceLimit)
            std::cout << "[transmog] Local resolver corrected actor=" << psprecomp::hex32(actor)
                      << " kind=" << kind << " donor-id=" << donor.id
                      << " donor-gender=" << static_cast<unsigned>(donor.gender)
                      << " actor-gender=" << static_cast<unsigned>(actor_gender)
                      << " model=" << psprecomp::hex32(*model)
                      << " native-converted=" << psprecomp::hex32(converted_model)
                      << " donor-base=" << psprecomp::hex32(donor_base)
                      << " corrected-resource=" << psprecomp::hex32(desired_resource) << '\n' << std::flush;
    }
    ctx.gpr[2] = desired_resource;
    ctx.pc = caller;
}

struct RefreshActor {
    std::uint32_t address{};
    std::uint32_t slot{};
    std::uint32_t vtable{};
    std::uint16_t identity{};
    std::uint8_t channel{};
    std::uint8_t visible{};
    bool preserve_root{};
    std::uint32_t component_slot{};
    bool pose_rebind_done{};
    std::uint8_t resource_cache_trace_count{};
};
struct RefreshJob {
    psprecomp::Runtime *runtime{};
    psprecomp::AllegrexContext interrupted;
    std::uint64_t diagnostic_id{};
    std::int32_t thread{};
    std::uint32_t registry{};
    std::uint32_t pool{};
    std::uint32_t pool_accounting{};
    Set selection;
    std::vector<RefreshActor> actors;
    std::size_t index{};
    std::size_t stage{};
    bool phase_started{};
    std::chrono::steady_clock::time_point started;
};

void trace_animation_bindings(const char *phase, const RefreshActor &actor, const psprecomp::GuestMemory &memory) {
    if (std::getenv("MHP3RD_TRACE_TRANSMOG") == nullptr) return;
    const auto vtable = memory.load32(actor.address);
    if (!memory.contains(vtable + 68u, 4u))
        throw psprecomp::Error("[transmog] Hunter vtable slot +68 is outside guest RAM while tracing animation state.");
    std::cout << "[transmog] Native pose target " << phase << " actor="
              << psprecomp::hex32(actor.address) << " vtable=" << psprecomp::hex32(vtable)
              << " slot68=" << psprecomp::hex32(memory.load32(vtable + 68u))
              << " draw=" << static_cast<unsigned>(memory.load8(actor.address + 5152u)) << '\n';
    const auto render_group = actor.address + 3968u;
    if (!memory.contains(render_group + 72u, 4u) || !memory.contains(actor.address + 2992u, 4u) ||
        !memory.contains(actor.address + 398u, 2u) || !memory.contains(actor.address + 408u, 12u))
        throw psprecomp::Error("[transmog] Actor render-group or pose-mode field is outside guest RAM while tracing.");
    const auto render_vtable = memory.load32(render_group);
    if (!memory.contains(vtable + 32u, 4u))
        throw psprecomp::Error("[transmog] Actor vtable slot +32 is outside guest RAM while tracing.");
    std::cout << "[transmog] Pose dispatch " << phase << " actor=" << psprecomp::hex32(actor.address)
              << " getter-result=" << psprecomp::hex32(render_group)
              << " object-vtable=" << psprecomp::hex32(render_vtable)
              << " actor-slot32=" << psprecomp::hex32(memory.load32(vtable + 32u))
              << " actor-mode=" << psprecomp::hex32(memory.load16(actor.address + 398u))
              << " f408=" << psprecomp::hex32(memory.load32(actor.address + 408u))
              << " f412=" << psprecomp::hex32(memory.load32(actor.address + 412u))
              << " arg416=" << psprecomp::hex32(memory.load32(actor.address + 416u))
              << " update-flags=" << psprecomp::hex32(memory.load32(actor.address + 2992u))
              << " render-group-count=" << psprecomp::hex32(memory.load32(render_group + 72u)) << '\n';
    const auto render_child = memory.load32(render_group + 88u);
    std::cout << "[transmog] Render child " << phase << " actor=" << psprecomp::hex32(actor.address)
              << " pointer=" << psprecomp::hex32(render_child);
    if (render_child != 0u) {
        if (!memory.contains(render_child, 642u))
            throw psprecomp::Error("[transmog] Render-child state pointer " + psprecomp::hex32(render_child) +
                " is outside guest RAM while tracing.");
        std::cout << " vtable=" << psprecomp::hex32(memory.load32(render_child))
                  << " list324=" << psprecomp::hex32(memory.load32(render_child + 324u))
                  << " list328=" << psprecomp::hex32(memory.load32(render_child + 328u))
                  << " list332=" << psprecomp::hex32(memory.load32(render_child + 332u))
                  << " slot336=" << psprecomp::hex32(memory.load32(render_child + 336u))
                  << " slot480=" << psprecomp::hex32(memory.load32(render_child + 480u))
                  << " list624=" << psprecomp::hex32(memory.load32(render_child + 624u));
        for (const auto offset : {336u, 480u}) {
            std::cout << " descriptor" << offset << '=';
            for (std::uint32_t byte = 0u; byte < 18u; ++byte)
                std::cout << psprecomp::hex32(memory.load8(render_child + offset + byte)) << ',';
        }
    }
    std::cout << '\n';
    std::cout << "[transmog] Animation descriptors " << phase << " actor="
              << psprecomp::hex32(actor.address) << " mask="
              << psprecomp::hex32(memory.load32(actor.address + 3548u)) << " table="
              << psprecomp::hex32(memory.load32(actor.address + 3480u)) << " render-selector="
              << static_cast<unsigned>(memory.load8(actor.address + 1504u));
    const auto table = memory.load32(actor.address + 3480u);
    if (memory.contains(table, 16u))
        std::cout << " table-entry=" << psprecomp::hex32(memory.load32(table + 12u));
    for (std::uint32_t index = 0u; index < 8u; ++index) {
        const auto descriptor = actor.address + 3584u + index * 48u;
        std::cout << " d" << index << "={" << static_cast<unsigned>(memory.load8(descriptor))
                  << ',' << psprecomp::hex32(memory.load32(descriptor + 4u))
                  << ',' << psprecomp::hex32(memory.load32(descriptor + 8u))
                  << ',' << psprecomp::hex32(memory.load32(descriptor + 12u))
                  << ',' << psprecomp::hex32(memory.load32(descriptor + 16u)) << '}';
    }
    std::cout << '\n' << std::flush;
    for (std::uint32_t index = 0u; index < 7u; ++index) {
        const auto entry = actor.address + 4056u + index * 128u;
        if (!memory.contains(entry, 4u))
            throw psprecomp::Error("[transmog] Hunter visual-object table extends outside guest RAM while tracing.");
        const auto pointer = memory.load32(entry);
        std::cout << "[transmog] Visual object slot " << phase << " actor="
                  << psprecomp::hex32(actor.address) << " index=" << index
                  << " entry=" << psprecomp::hex32(entry) << " pointer=" << psprecomp::hex32(pointer);
        if (pointer != 0u) {
            if (!memory.contains(pointer, 64u))
                throw psprecomp::Error("[transmog] Hunter visual-object pointer " + psprecomp::hex32(pointer) +
                    " is outside guest RAM while tracing.");
            for (std::uint32_t word = 0u; word < 16u; ++word)
                std::cout << " " << psprecomp::hex32(memory.load32(pointer + word * 4u));
        }
        std::cout << '\n';
    }
    std::cout << std::flush;
}

void trace_guest_context(const char *phase, const psprecomp::AllegrexContext &ctx) {
    if (std::getenv("MHP3RD_TRACE_TRANSMOG") == nullptr) return;
    std::cout << "[transmog] Guest-call context " << phase
              << " sp=" << psprecomp::hex32(ctx.gpr[29])
              << " ra=" << psprecomp::hex32(ctx.gpr[31])
              << " vfpu=" << psprecomp::hex32(ctx.vfpu_ctrl[0]) << ','
              << psprecomp::hex32(ctx.vfpu_ctrl[1]) << ','
              << psprecomp::hex32(ctx.vfpu_ctrl[2]) << ','
              << psprecomp::hex32(ctx.vfpu_ctrl[3]) << '\n' << std::flush;
}

void trace_native_update_state(const char *phase, const RefreshActor &actor, const psprecomp::GuestMemory &memory) {
    if (std::getenv("MHP3RD_TRACE_TRANSMOG") == nullptr) return;
    const auto flags = actor.address + 2992u;
    const auto group_count = actor.address + 3968u + 72u;
    if (!memory.contains(flags, 4u) || !memory.contains(group_count, 4u))
        throw psprecomp::Error("[transmog] Native pose-update fields are outside guest RAM while tracing hunter " +
            psprecomp::hex32(actor.address) + ".");
    std::cout << "[transmog] Native pose-update state " << phase << " actor=" << psprecomp::hex32(actor.address)
              << " flags=" << psprecomp::hex32(memory.load32(flags))
              << " render-group-count=" << psprecomp::hex32(memory.load32(group_count)) << '\n' << std::flush;
}

void verify_refresh_actor(const RefreshJob &job) {
    const auto &memory = job.runtime->memory();
    const auto &actor = job.actors.at(job.index);
    if (kernel().current_uid() != job.thread || kernel().in_interrupt() ||
        memory.load32(kHunterRegistry) != job.registry || memory.load32(kVisualPool) != job.pool ||
        memory.load32(job.registry + 8u + actor.slot * 4u) != actor.address ||
        memory.load32(actor.address) != actor.vtable ||
        memory.load16(actor.address + 96u) != actor.identity ||
        memory.load8(actor.address + 99u) != actor.channel ||
        (memory.load32(actor.address + 4u) & 1u) == 0u || memory.load8(actor.address + 5154u) == 0u)
        throw psprecomp::Error("[transmog] Hunter ownership changed during native visual refresh; stop and reload the hunter before applying another set.");
    if (std::chrono::steady_clock::now() - job.started > std::chrono::seconds(30))
        throw psprecomp::Error("[transmog] Native visual refresh did not complete within 30 seconds; check the game asset files and reload the hunter.");
}

void continue_refresh(const std::shared_ptr<RefreshJob> &job, psprecomp::AllegrexContext &ctx);

void install_components_preserving_root(const std::shared_ptr<RefreshJob> &job, psprecomp::AllegrexContext &ctx) {
    verify_refresh_actor(*job);
    const auto &actor = job->actors[job->index];
    if (actor.component_slot < 7u) {
        const auto slot = actor.component_slot;
        trace_guest_context(("component slot " + std::to_string(slot)).c_str(), ctx);
        kernel().call_guest(ctx, kInstallArmorComponent, {actor.address, slot, 0u, 0u},
            [job](psprecomp::AllegrexContext &returned, std::uint32_t) {
                verify_refresh_actor(*job);
                ++job->actors[job->index].component_slot;
                install_components_preserving_root(job, returned);
            });
        return;
    }
    trace_guest_context("component finalize", ctx);
    kernel().call_guest(ctx, kFinalizeArmorComponents, {actor.address, 0u, 0u, 0u},
        [job](psprecomp::AllegrexContext &returned, std::uint32_t) {
            verify_refresh_actor(*job);
            auto &memory = job->runtime->memory();
            const auto &finished_actor = job->actors[job->index];
            if (memory.load32(finished_actor.address + 2944u) != 4u)
                throw psprecomp::Error("[transmog] Native armor-component installation returned without ready models for hunter " + psprecomp::hex32(finished_actor.address) + ".");
            memory.store32(finished_actor.address + 5148u, 127u);
            job->stage = 6u;
            continue_refresh(job, returned);
        });
}

void continue_refresh(const std::shared_ptr<RefreshJob> &job, psprecomp::AllegrexContext &ctx) {
    verify_refresh_actor(*job);
    auto &memory = job->runtime->memory();
    const auto &actor = job->actors[job->index];
    if (job->stage == 6u) {
        if (actor.vtable == kLocalHunterVtable && !job->actors[job->index].pose_rebind_done) {
            if (memory.load32(actor.address + 2944u) != 4u)
                throw psprecomp::Error("[transmog] Local hunter models are not ready for native pose binding.");
            const auto render_group = actor.address + 3968u;
            if (!memory.contains(render_group + 88u, 4u))
                throw psprecomp::Error("[transmog] Local hunter render-child pointer is outside guest RAM before native pose binding.");
            const auto render_child = memory.load32(render_group + 88u);
            if (render_child == 0u || !memory.contains(render_child, 642u))
                throw psprecomp::Error("[transmog] Local hunter has no valid render-child state for native pose binding.");
            const auto mode = memory.load16(actor.address + 398u);
            const auto f408 = memory.load32(actor.address + 408u);
            const auto f412 = memory.load32(actor.address + 412u);
            const auto arg416 = memory.load32(actor.address + 416u);
            trace_native_update_state("before native pose bind", actor, memory);
            if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
                std::cout << "[transmog] Native pose binding actor=" << psprecomp::hex32(actor.address)
                          << " mode=" << psprecomp::hex32(mode)
                          << " f408=" << psprecomp::hex32(f408)
                          << " f412=" << psprecomp::hex32(f412)
                          << " arg416=" << psprecomp::hex32(arg416)
                          << " render-child=" << psprecomp::hex32(render_child) << '\n' << std::flush;
            ctx.fpr[12] = std::bit_cast<float>(f408);
            ctx.fpr[13] = std::bit_cast<float>(f412);
            job->actors[job->index].pose_rebind_done = true;
            kernel().call_guest(ctx, kRefreshPoseState, {actor.address, mode, arg416, 0u},
                [job](psprecomp::AllegrexContext &returned, std::uint32_t result) {
                    verify_refresh_actor(*job);
                    const auto &refreshed_actor = job->actors[job->index];
                    trace_native_update_state("after native pose bind", refreshed_actor, job->runtime->memory());
                    if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
                        std::cout << "[transmog] Native pose binding complete actor="
                                  << psprecomp::hex32(refreshed_actor.address)
                                  << " result=" << psprecomp::hex32(result) << '\n' << std::flush;
                    continue_refresh(job, returned);
                });
            return;
        }
        trace_animation_bindings("after install", actor, memory);
        if (memory.load32(actor.address + 2944u) != 4u)
            throw psprecomp::Error("[transmog] Native visual installation returned without ready models for hunter " + psprecomp::hex32(actor.address) + ".");
        memory.store8(actor.address + 5152u, actor.visible);
        trace_animation_bindings("after visibility restore", actor, memory);
        ++job->index;
        job->stage = 0u;
        job->phase_started = false;
        if (job->index == job->actors.size()) {
            current_refresh.reset();
            const Set desired = enabled() ? current_selection() : Set{};
            observed_selection = desired;
            refresh_pending = desired != job->selection;
            refresh_message = refresh_pending ? "Applying the updated appearances..." : "Appearances applied.";
            if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
                std::cout << "[transmog] Visual refresh complete job=" << job->diagnostic_id
                          << " for " << job->actors.size() << " hunter(s)"
                          << " callback-pc=" << psprecomp::hex32(ctx.pc)
                          << " callback-ra=" << psprecomp::hex32(ctx.gpr[31])
                          << " callback-sp=" << psprecomp::hex32(ctx.gpr[29])
                          << " resume-pc=" << psprecomp::hex32(job->interrupted.pc)
                          << " resume-ra=" << psprecomp::hex32(job->interrupted.gpr[31])
                          << " resume-sp=" << psprecomp::hex32(job->interrupted.gpr[29]) << '\n' << std::flush;
                std::cout << "[transmog] Visual pool accounting start=" << job->pool_accounting
                          << " end=" << memory.load32(job->pool + 624u) << '\n' << std::flush;
            }
            ctx = job->interrupted;
            if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
                std::cout << "[transmog] Restored frame continuation job=" << job->diagnostic_id
                          << " thread=" << kernel().current_uid()
                          << " pc=" << psprecomp::hex32(ctx.pc)
                          << " ra=" << psprecomp::hex32(ctx.gpr[31]) << '\n' << std::flush;
            }
            return;
        }
        continue_refresh(job, ctx);
        return;
    }
    if (job->stage == 0u) memory.store8(actor.address + 5152u, 0u);
    if (job->stage == 2u && actor.preserve_root) {
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
            std::cout << "[transmog] Hunter " << psprecomp::hex32(actor.address)
                      << ": preserve unchanged weapon resource and root rig\n" << std::flush;
        trace_native_update_state("root cleanup skipped to preserve weapon rig", actor, memory);
        ++job->stage;
        job->phase_started = false;
        continue_refresh(job, ctx);
        return;
    }
    if (job->stage == 5u && actor.preserve_root) {
        if (!job->phase_started) {
            if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
                std::cout << "[transmog] Hunter " << psprecomp::hex32(actor.address)
                          << ": install seven armor components and retain root rig\n" << std::flush;
            job->phase_started = true;
        }
        install_components_preserving_root(job, ctx);
        return;
    }
    const auto stage = job->stage;
    if (!job->phase_started) {
        constexpr std::array<std::string_view, 6> phases{
            "reset asset loading", "cleanup armor buffers", "cleanup root buffers",
            "cache model indices", "load visual assets", "install visual models"};
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
            std::cout << "[transmog] Hunter " << psprecomp::hex32(actor.address)
                      << ": " << phases[stage] << '\n' << std::flush;
        job->phase_started = true;
    }
    const auto arguments = stage == 1u || stage == 2u
                                    ? std::array<std::uint32_t, 4>{job->pool, actor.address, 0u, 0u}
                                    : std::array<std::uint32_t, 4>{actor.address, 0u, 0u, 0u};
    trace_guest_context(("refresh stage " + std::to_string(stage)).c_str(), ctx);
    // Local hunters resolve equipped armor through their identity; the standard
    // hunter cache helper instead reads its embedded equipment records.
    const auto function = stage == 3u && actor.vtable == kLocalHunterVtable
        ? kLocalCacheModelIndices : kRefreshFunctions[stage];
    kernel().call_guest(ctx, function, arguments,
        [job, stage](psprecomp::AllegrexContext &returned, std::uint32_t) {
            verify_refresh_actor(*job);
            if (stage == 2u && std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
                std::cout << "[transmog] Visual pool accounting after root cleanup="
                          << job->runtime->memory().load32(job->pool + 624u) << '\n' << std::flush;
            if (stage == 1u)
                trace_native_update_state("after armor cleanup", job->actors[job->index], job->runtime->memory());
            if (stage == 2u)
                trace_native_update_state("after root cleanup", job->actors[job->index], job->runtime->memory());
            if (stage == 4u) {
                auto &memory = job->runtime->memory();
                auto &actor = job->actors[job->index];
                const auto state = memory.load32(actor.address + 2944u);
                if (state > 3u)
                    throw psprecomp::Error("[transmog] Native resource loader entered unexpected visual state " + std::to_string(state) + ".");
                if (actor.vtable == kLocalHunterVtable && std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
                    if (!memory.contains(actor.address + 2948u, 8u) || !memory.contains(actor.address + 1112u, 1u))
                        throw psprecomp::Error("[transmog] Local hunter resource cache or gender is outside guest RAM after native load for hunter " + psprecomp::hex32(actor.address) + ".");
                    const auto cursor = memory.load32(actor.address + 2948u);
                    constexpr std::uint8_t kResourceCacheTraceLimit = 64u;
                    if (cursor == 4u || state == 3u || actor.resource_cache_trace_count == 0u) {
                        if (actor.resource_cache_trace_count < kResourceCacheTraceLimit) {
                            ++actor.resource_cache_trace_count;
                            std::cout << "[transmog] Local native resource cache callback actor=" << psprecomp::hex32(actor.address)
                                  << " callback=" << static_cast<unsigned>(actor.resource_cache_trace_count)
                                  << " cursor=" << cursor
                                  << " resource=" << psprecomp::hex32(memory.load32(actor.address + 2952u))
                                  << " gender=" << static_cast<unsigned>(memory.load8(actor.address + 1112u))
                                  << " visual-state=" << state << '\n' << std::flush;
                        } else if (actor.resource_cache_trace_count == kResourceCacheTraceLimit) {
                            ++actor.resource_cache_trace_count;
                            std::cout << "[transmog] Local native resource-cache trace reached its 64-callback limit for actor="
                                      << psprecomp::hex32(actor.address) << '\n' << std::flush;
                        }
                    }
                }
                if (state != 3u) {
                    kernel().delay_guest_callback(returned, 1000u,
                        [job](psprecomp::AllegrexContext &resumed, std::uint32_t) { continue_refresh(job, resumed); });
                    return;
                }
            }
            ++job->stage;
            job->phase_started = false;
            continue_refresh(job, returned);
        });
}
} // namespace

std::string refresh_status() { return refresh_message; }
void ge_list_started(std::uint32_t id, std::uint32_t start, std::int32_t thread) {
    if (!ge_refresh_fence || ge_refresh_fence->matched || thread != ge_refresh_fence->thread) return;
    auto &fence = *ge_refresh_fence;
    if (start != fence.head) return;
    fence.list_id = id;
    fence.matched = true;
    if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
        std::cout << "[transmog] GE fence matched list=" << id << " start=" << psprecomp::hex32(start)
                  << " owner=" << thread << '\n' << std::flush;
}
void ge_list_finished(std::uint32_t id, bool done) {
    if (ge_refresh_fence && ge_refresh_fence->matched && ge_refresh_fence->list_id == id)
        ge_refresh_fence->done = done;
}
void ge_finish_queued(std::uint32_t id) {
    if (ge_refresh_fence && ge_refresh_fence->matched && ge_refresh_fence->list_id == id)
        ge_refresh_fence->finish_queued = true;
}
void ge_finish_returned(std::uint32_t id) {
    if (ge_refresh_fence && ge_refresh_fence->matched && ge_refresh_fence->list_id == id) {
        ge_refresh_fence->finish_returned = true;
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
            std::cout << "[transmog] GE fence finish callback returned list=" << id << '\n' << std::flush;
    }
}
void frame(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
        static bool traced_frame{};
        if (!traced_frame) {
            traced_frame = true;
            std::cout << "[transmog] Runtime frame callback reached; uid=" << kernel().current_uid()
                      << " interrupt=" << kernel().in_interrupt() << '\n' << std::flush;
        }
    }
    if (installed_runtime != &runtime || kernel().current_thread() == nullptr || kernel().in_interrupt()) return;
    migrate_legacy_if_ready();
    if (current_refresh) {
        const auto *owner = kernel().find_thread(current_refresh->thread);
        if (owner == nullptr || owner->status == ThreadStatus::Dormant || owner->status == ThreadStatus::Dead) {
            const auto &memory = runtime.memory();
            const auto registry = memory.load32(kHunterRegistry);
            if (registry != 0u && !memory.contains(registry, 32u))
                throw psprecomp::Error("[transmog] Hunter registry is invalid after the refresh thread exited.");
            if (registry != 0u) {
                for (std::uint32_t slot = 0u; slot < 6u; ++slot) {
                    const auto address = memory.load32(registry + 8u + slot * 4u);
                    if (std::any_of(current_refresh->actors.begin(), current_refresh->actors.end(),
                        [address](const RefreshActor &actor) { return actor.address == address; }))
                        throw psprecomp::Error("[transmog] The refresh thread exited while a tracked hunter address remained registered; its model ownership cannot be recovered safely. Reload the hunter before applying another set.");
                }
            }
            // The engine already detached every affected actor. Their former
            // memory and the dead thread's saved context must remain untouched.
            refresh_pending = true;
            current_refresh.reset();
            refresh_message = "The hunter unloaded during refresh; appearances will apply to the next ready hunter.";
            std::cerr << "[transmog] " << refresh_message << '\n';
        }
    }
    Set desired;
    if (settings::current().layered_armor && saved_appearance_problem.empty() && settings_load_problem.empty()) {
        ensure_settings();
        if (saved_appearance_problem.empty()) desired = current_selection();
    }
    if (desired != observed_selection) {
        observed_selection = desired;
        refresh_pending = true;
        refresh_message = "Applying appearances when the hunter is ready...";
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
            std::cout << "[transmog] Runtime frame observed an appearance selection change\n" << std::flush;
    }
    if (!refresh_pending || current_refresh || (ge_refresh_fence && !ge_refresh_ready)) return;
    auto &memory = runtime.memory();
    const auto registry = memory.load32(kHunterRegistry);
    const auto pool = memory.load32(kVisualPool);
    if (registry == 0u || pool == 0u) {
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
            static bool traced_missing_runtime_state{};
            if (!traced_missing_runtime_state) {
                traced_missing_runtime_state = true;
                std::cout << "[transmog] Refresh pending; registry=" << psprecomp::hex32(registry)
                          << " pool=" << psprecomp::hex32(pool) << '\n' << std::flush;
            }
        }
        return;
    }
    if (!memory.contains(registry, 32u) || !memory.contains(pool, 628u))
        throw psprecomp::Error("[transmog] Native hunter registry or visual pool is outside guest RAM.");
    ReadOnlyRam appearance_memory(memory);
    if (!character_loaded(appearance_memory)) {
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
            static bool traced_unloaded_character{};
            if (!traced_unloaded_character) {
                traced_unloaded_character = true;
                std::cout << "[transmog] Refresh pending until the loaded-hunter marker is ready\n" << std::flush;
            }
        }
        return;
    }
    auto job = std::make_shared<RefreshJob>();
    static std::uint64_t next_diagnostic_id{};
    job->diagnostic_id = ++next_diagnostic_id;
    job->runtime = &runtime;
    job->interrupted = ctx;
    job->interrupted.pc = ge_refresh_ready ? ctx.pc : ctx.gpr[31];
    job->thread = kernel().current_uid();
    job->registry = registry;
    job->pool = pool;
    job->pool_accounting = memory.load32(pool + 624u);
    job->selection = desired;
    job->started = std::chrono::steady_clock::now();
    trace_guest_context("refresh job start", ctx);
    for (std::uint32_t slot = 0u; slot < 6u; ++slot) {
        const auto actor = memory.load32(registry + 8u + slot * 4u);
        if (actor == 0u) continue;
        if (!memory.contains(actor, 5300u))
            throw psprecomp::Error("[transmog] Registered hunter pointer is outside guest RAM at slot " + std::to_string(slot) + ".");
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
            using ActorTrace = std::array<std::uint32_t, 5>;
            static std::array<ActorTrace, 6> previous{};
            const ActorTrace observed{actor, memory.load32(actor), memory.load32(actor + 4u),
                memory.load8(actor + 5154u), memory.load32(actor + 2944u)};
            if (observed != previous[slot]) {
                previous[slot] = observed;
                std::cout << "[transmog] Registry slot=" << slot << " actor=" << psprecomp::hex32(actor)
                          << " vtable=" << psprecomp::hex32(observed[1]) << " active=" << observed[2]
                          << " drawable=" << observed[3] << " visual-state=" << observed[4] << '\n' << std::flush;
            }
        }
        const auto vtable = memory.load32(actor);
        if ((vtable != kHunterVtable && vtable != kLocalHunterVtable) || (memory.load32(actor + 4u) & 1u) == 0u ||
            memory.load8(actor + 5154u) == 0u) continue;
        const auto state = memory.load32(actor + 2944u);
        if (state < 4u) {
            if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
                static bool traced_unready_actor{};
                if (!traced_unready_actor) {
                    traced_unready_actor = true;
                    std::cout << "[transmog] Refresh pending; hunter " << psprecomp::hex32(actor)
                              << " visual state=" << state << '\n' << std::flush;
                }
            }
            return;
        }
        if (state > 4u)
            throw psprecomp::Error("[transmog] Registered hunter has unsupported ready visual state " + std::to_string(state) + ".");
        const auto weapon_resource = weapon::effective_resource_id_for_actor(
            appearance_memory, actor, desired.weapon,
            desired.weapon ? equipment_count(desired.weapon->kind) : 0u);
        const auto cached_weapon_model = memory.load32(actor + 2984u);
        const bool preserve_root = weapon_resource == cached_weapon_model;
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
            std::cout << "[transmog] Hunter " << psprecomp::hex32(actor)
                      << " slot7 desired-resource=" << psprecomp::hex32(weapon_resource)
                      << " cached=" << psprecomp::hex32(cached_weapon_model)
                      << " preserve-root=" << preserve_root << '\n' << std::flush;
        job->actors.push_back({actor, slot, vtable, memory.load16(actor + 96u), memory.load8(actor + 99u),
            memory.load8(actor + 5152u), preserve_root, 0u});
        trace_animation_bindings("before cleanup", job->actors.back(), memory);
    }
    if (job->actors.empty()) {
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr) {
            static bool traced_no_hunters{};
            if (!traced_no_hunters) {
                traced_no_hunters = true;
                std::cout << "[transmog] Refresh pending; no eligible hunter in registry "
                          << psprecomp::hex32(registry) << '\n' << std::flush;
            }
        }
        return;
    }
    if (enabled()) {
        // Persisted invalid weapon donors have a visible stock-weapon recovery
        // in the weapon hook. Keep armor loading active and leave that donor
        // unchanged so the menu can reset or correct it.
        auto armor_selection = desired;
        armor_selection.weapon.reset();
        try {
            validate_set(armor_selection);
        } catch (const psprecomp::Error &error) {
            saved_appearance_problem = std::string(error.what()) +
                " Original appearances are active until this set is reset or corrected.";
            std::cerr << "[transmog] " << saved_appearance_problem << '\n';
            job->selection = {};
            refresh_message = saved_appearance_problem;
        }
    }
    for (const auto function : kRefreshFunctions)
        if (!runtime.has_function(function))
            throw psprecomp::Error("[transmog] Native visual refresh function is absent at " + psprecomp::hex32(function) + ".");
    for (const auto &actor : job->actors) {
        if (actor.vtable == kLocalHunterVtable && !runtime.has_function(kLocalCacheModelIndices))
            throw psprecomp::Error("[transmog] Native local hunter model cache helper is absent; restore the supported executable.");
        if (actor.preserve_root && (!runtime.has_function(kInstallArmorComponent) || !runtime.has_function(kFinalizeArmorComponents)))
            throw psprecomp::Error("[transmog] Native component-only armor installer is absent; restore the supported executable.");
    }
    if (!ge_refresh_ready) {
        const auto manager = ctx.gpr[16];
        if (!memory.contains(manager, 168u) || memory.load32(manager) > 1u)
            throw psprecomp::Error("[transmog] Cannot fence visual refresh: display-list manager is invalid at " +
                psprecomp::hex32(manager) + ".");
        const auto head = manager + memory.load32(manager) * 160u + 4u;
        ge_refresh_fence = GeRefreshFence{job->thread, head, 0u, kernel().now_us() + 30000000u};
        refresh_message = "Waiting for the current display list before applying appearances...";
        if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
            std::cout << "[transmog] GE fence armed owner=" << job->thread << " head="
                      << psprecomp::hex32(head)
                      << '\n' << std::flush;
        return;
    }
    current_refresh = job;
    refresh_message = "Loading appearances...";
    if (std::getenv("MHP3RD_TRACE_TRANSMOG") != nullptr)
        std::cout << "[transmog] Starting visual refresh on thread " << job->thread
                  << " for " << job->actors.size() << " hunter(s)\n" << std::flush;
    continue_refresh(job, ctx);
}

std::optional<std::uint16_t> select_appearance(
    const game::Ram &ram, const Set &selection, const std::array<std::size_t, 5> &counts,
    std::uint32_t kind, std::uint32_t gender) {
    if (kind >= 5u) return std::nullopt;
    const auto slot = kind == 4u ? 0u : kind + 1u;
    if (!selection.armor[slot]) return std::nullopt;
    const auto donor = *selection.armor[slot];
    gender = donor.gender;
    if (gender > 1u)
        throw psprecomp::Error("[transmog] Appearance lookup received unsupported gender " + std::to_string(gender) + ".");
    if (counts[slot] == 0u || counts[slot] > 4096u || donor.id >= counts[slot])
        throw psprecomp::Error("[transmog] Saved " + std::string(kSlots[slot]) + " appearance id " + std::to_string(donor.id) +
            " is outside the loaded armor catalog (" + std::to_string(counts[slot]) + " entries). Reset that appearance or select a supported armor piece.");
    const auto address = kTables[slot] + static_cast<std::uint32_t>(donor.id) * 0x28u + gender * 2u;
    if (!ram.contains(address, 2u))
        throw psprecomp::Error("[transmog] " + std::string(kSlots[slot]) + " model lookup is outside guest RAM; restore the supported executable.");
    const auto model = ram.load16(address);
    if (donor.id != 0u && model == 0u)
        throw psprecomp::Error("[transmog] Saved " + std::string(kSlots[slot]) + " appearance id " + std::to_string(donor.id) +
            " has no model for donor gender " + std::to_string(gender) + ". Reset that appearance or select a supported donor.");
    return model;
}

bool available() { return unavailable_reason().empty(); }
bool enabled() {
    if (current_refresh) return true;
    if (installed_runtime == nullptr || !settings::current().layered_armor || !saved_appearance_problem.empty() || !settings_load_problem.empty()) return false;
    ensure_settings();
    return saved_appearance_problem.empty();
}
const Set &visual_selection() { return current_refresh ? current_refresh->selection : current_selection(); }

namespace {
std::uint32_t equipment_name_table(std::uint8_t kind) {
    constexpr std::array<std::pair<std::uint8_t, std::uint32_t>, 12> tables{{
        {5u,5u},{12u,17u},{6u,7u},{16u,25u},{7u,9u},{17u,27u},
        {8u,11u},{14u,21u},{13u,19u},{11u,15u},{9u,13u},{15u,23u}}};
    const auto found = std::find_if(tables.begin(), tables.end(), [kind](const auto &entry) { return entry.first == kind; });
    if (found == tables.end()) throw psprecomp::Error("[transmog] Names requested for unsupported weapon class " + std::to_string(kind) + ".");
    return found->second;
}
}

std::size_t equipment_count(std::uint8_t kind) {
    if (installed_runtime == nullptr) throw psprecomp::Error("[transmog] Equipment count requested before runtime installation.");
    ReadOnlyRam ram(installed_runtime->memory());
    return name_directory(ram, equipment_name_table(kind)).count;
}

std::vector<std::string> equipment_names(std::uint8_t kind) {
    if (installed_runtime == nullptr) throw psprecomp::Error("[transmog] Equipment names requested before runtime installation.");
    ReadOnlyRam ram(installed_runtime->memory());
    return names_table(ram, equipment_name_table(kind));
}

std::string unavailable_reason() {
    if (installed_runtime == nullptr) return installation_problem;
    if (!settings::current().layered_armor) return "Turn on Layered Sets in Mods to edit appearances.";
    ensure_settings();
    if (!settings_load_problem.empty()) return settings_load_problem;
    if (!saved_appearance_problem.empty()) return saved_appearance_problem;
    ReadOnlyRam ram(installed_runtime->memory());
    if (!character_loaded(ram)) return "Load a hunter to choose armor appearances.";
    const auto counts = catalog_counts(ram);
    if (std::any_of(counts.begin(), counts.end(), [](std::size_t count) { return count == 0u; }))
        return "The game's armor names have not loaded yet.";
    return {};
}

std::vector<Armor> armor_list(std::size_t slot) {
    if (slot >= kSlots.size()) throw psprecomp::Error("[transmog] Armor catalog requested invalid slot " + std::to_string(slot) + ".");
    if (installed_runtime == nullptr) return {};
    ReadOnlyRam ram(installed_runtime->memory());
    if (!character_loaded(ram)) return {};
    const auto names = armor_names(ram, slot);
    if (names.size() > 4096u) throw psprecomp::Error("[transmog] Armor name table exceeds the supported catalog bound.");
    std::vector<Armor> result;
    for (std::size_t id = 0; id < names.size(); ++id) {
      for (std::uint32_t gender = 0; gender < 2u; ++gender) {
        const auto address = kTables[slot] + static_cast<std::uint32_t>(id) * 0x28u + gender * 2u;
        if (!ram.contains(address, 2u)) throw psprecomp::Error("[transmog] Armor name/model tables disagree; restore the supported executable.");
        if (id == 0u) result.push_back({0u, "No armor", static_cast<std::uint8_t>(gender)});
        else if (!names[id].empty() && ram.load16(address) != 0u)
            result.push_back({static_cast<std::uint16_t>(id), names[id], static_cast<std::uint8_t>(gender)});
      }
    }
    return result;
}

void install(psprecomp::Runtime &runtime, AppearanceHooks functions, std::string_view executable_sha256) {
    const char *disabled = std::getenv("MHP3RD_NO_TRANSMOG");
    if (disabled != nullptr && std::string_view(disabled) != "0" && std::string_view(disabled) != "1")
        throw psprecomp::Error("MHP3RD_NO_TRANSMOG must be 0 or 1; unset it to enable the layered armor runtime.");
    if (disabled != nullptr && std::string_view(disabled) == "1") {
        runtime_disabled = true;
        installation_problem = "Layered armor is disabled by MHP3RD_NO_TRANSMOG=1.";
        std::cout << "[transmog] " << installation_problem << '\n';
        return;
    }
    ensure_settings();
    if (executable_sha256 != kExecutable)
        throw psprecomp::Error("[transmog] Unsupported executable SHA-256 " + std::string(executable_sha256) + "; restore the supported NPJB-40001 executable or set MHP3RD_NO_TRANSMOG=1.");
    if (functions.model == nullptr || functions.converter == nullptr || !runtime.has_function(kHelper) ||
        !runtime.has_function(kConverter) || installed_runtime == &runtime)
        throw psprecomp::Error("[transmog] Original appearance getter is absent or already wrapped; install once after generated functions.");
    for (const auto &word : kSignature)
        if (runtime.memory().load32(word.address) != word.word)
            throw psprecomp::Error("[transmog] Unsupported appearance getter code at " + psprecomp::hex32(word.address) + "; restore the supported executable or set MHP3RD_NO_TRANSMOG=1.");
    original = functions.model;
    original_converter = functions.converter;
    installed_runtime = &runtime;
    psprecomp::set_runtime_post_dispatch_hook(&post_dispatch_refresh);
    runtime.register_function(kHelper, &appearance, "mhp3rd_layered_armor_model_index");
    runtime.register_function(kConverter, &convert_appearance, "mhp3rd_layered_armor_resource");
    if (!runtime.has_function(kLocalHelper))
        throw psprecomp::Error("[transmog] Local hunter armor getter is absent at " + psprecomp::hex32(kLocalHelper) + ".");
    runtime.register_function(kLocalHelper, &local_appearance, "mhp3rd_layered_local_armor_model_index");
    if (!runtime.has_function(kLocalResourceResolver))
        throw psprecomp::Error("[transmog] Local hunter armor resolver is absent at " + psprecomp::hex32(kLocalResourceResolver) + ".");
    runtime.register_function(kLocalResourceResolver, &local_resource_resolver, "mhp3rd_layered_local_armor_resource");
    installation_problem.clear();
    std::cout << "[transmog] Visual model selector installed; gameplay equipment and armor tables remain original\n";
}
} // namespace mhp3rd::mods::transmog
