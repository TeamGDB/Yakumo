#include "mods/transmog_weapon.hpp"
#include "game/guest_ram.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
namespace weapon = mhp3rd::mods::transmog::weapon;
namespace transmog = mhp3rd::mods::transmog;
using mhp3rd::game::BufferRam;
unsigned failures{};
bool feature_enabled{};
unsigned name_reads{};
transmog::Store *fixture_store{};
std::optional<transmog::Set> rebuilding;
bool feature() { return feature_enabled; }
const transmog::Set &selection() { return rebuilding ? *rebuilding : fixture_store->active(); }
std::vector<std::string> names(std::uint8_t) { ++name_reads; return {"None", "First donor", "Second donor"}; }
std::size_t count(std::uint8_t) { return 3u; }
void original(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    if (ctx.pc == 0x0886A47Cu) {
        ctx.gpr[2] = 71u;
        ctx.gpr[9] = 91u;
        ctx.hi = 123u;
        ctx.pc = ctx.gpr[31];
        return;
    }
    if (ctx.pc == 0x0886A534u) {
        const auto record = ctx.gpr[4] + ctx.gpr[5] * 252u + 48u;
        ctx.gpr[6] = runtime.memory().load16(record + 78u);
        ctx.gpr[5] = runtime.memory().load8(record + 118u);
        ctx.gpr[2] = 71u;
        ctx.gpr[9] = 91u;
        ctx.hi = 123u;
        ctx.pc = ctx.gpr[31];
        return;
    }
    const auto record = ctx.gpr[5];
    ctx.gpr[6] = runtime.memory().load16(record + 2u);
    ctx.gpr[5] = runtime.memory().load8(record + 1u);
    ctx.gpr[2] = 71u;
    ctx.gpr[9] = 91u;
    ctx.hi = 123u;
    ctx.pc = ctx.gpr[31];
}
void check(bool success, std::string_view message) {
    if (!success) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class F> void expect_error(F operation, std::string_view message) {
    try { operation(); check(false, message); }
    catch (const psprecomp::Error &error) {
        check(std::string_view(error.what()).size() > 20u, "failure gives a specific diagnosis");
    }
}
std::vector<std::uint8_t> records(const BufferRam &ram) {
    std::vector<std::uint8_t> bytes;
    for (const auto &entry : weapon::model_tables())
        for (std::uint32_t offset = 0; offset < entry.stride * 3u; ++offset)
            bytes.push_back(ram.load8(entry.address + offset));
    return bytes;
}
}

int main() {
    BufferRam ram(0x08800000u, 0x01000000u);
    for (const auto &entry : weapon::model_tables())
        for (unsigned id = 0; id < 3u; ++id) {
            const auto address = entry.address + id * entry.stride;
            for (unsigned byte = 2u; byte < entry.stride; ++byte)
                ram.store8(address + byte, static_cast<std::uint8_t>(entry.kind + id + byte));
            ram.store16(address, static_cast<std::uint16_t>(entry.kind * 100u + id));
        }
    const auto baseline = records(ram);
    check(weapon::model_tables().size() == 12u, "all twelve native weapon classes have model metadata");
    for (const auto &entry : weapon::model_tables()) {
        check(!weapon::select_appearance(ram, std::nullopt, 3u, entry.kind), "Default preserves equipped weapon appearance");
        transmog::Set set{};
        set.armor[0] = transmog::ArmorAppearance{1u, 1u};
        set.armor[1] = transmog::ArmorAppearance{2u, 0u};
        set.weapon = transmog::WeaponAppearance{1u, entry.kind};
        const auto saved = set;
        check(weapon::select_appearance(ram, set.weapon, 3u, entry.kind) == entry.kind * 100u + 1u,
              "matching class uses its donor model");
        const auto other = entry.kind == 5u ? 12u : 5u;
        check(!weapon::select_appearance(ram, set.weapon, 3u, static_cast<std::uint8_t>(other)),
              "class mismatch delegates only the weapon appearance");
        check(set == saved, "class mismatch preserves every armor choice and saved weapon binding");
        check(weapon::select_appearance(ram, set.weapon, 3u, entry.kind) == entry.kind * 100u + 1u,
              "returning to matching class restores the saved appearance automatically");
        check(set == saved, "matching-class resumption does not rewrite choices");
    }
    check(records(ram) == baseline, "class and selection checks preserve every native weapon model table");
    constexpr std::uint32_t local_actor = 0x09000000u;
    constexpr std::uint32_t remote_actor = 0x09001000u;
    constexpr std::uint32_t records_table = 0x08AB3640u;
    constexpr std::uint32_t records_data = 0x08B00000u;
    ram.store32(records_table, records_data);
    ram.store32(local_actor, 0x0896FBC8u);
    ram.store16(local_actor + 96u, 3u);
    const auto local_record = records_data + 3u * 252u + 48u;
    ram.store16(local_record + 78u, 1u);
    ram.store8(local_record + 118u, 14u);
    ram.store8(local_actor + 98u, 9u);
    ram.store32(remote_actor, 0x0896FED8u);
    ram.store16(remote_actor + 1162u, 1u);
    ram.store8(remote_actor + 1161u, 15u);
    ram.store8(remote_actor + 98u, 9u);
    const auto local_table = std::find_if(weapon::model_tables().begin(), weapon::model_tables().end(),
        [](const weapon::ModelTable &entry) { return entry.kind == 14u; });
    const auto remote_table = std::find_if(weapon::model_tables().begin(), weapon::model_tables().end(),
        [](const weapon::ModelTable &entry) { return entry.kind == 15u; });
    ram.store16(local_table->address + local_table->stride, 9u);
    ram.store16(local_table->address + local_table->stride * 2u, 10u);
    ram.store16(remote_table->address + remote_table->stride, 20u);
    ram.store16(remote_table->address + remote_table->stride * 2u, 25u);
    ram.store16(0x089CD1F8u + 9u * 4u, 300u);
    ram.store16(0x089CD22Cu + 9u * 2u, 0x06E2u);
    const auto cached_root_baseline = records(ram);
    check(weapon::effective_model_for_actor(ram, local_actor, transmog::WeaponAppearance{2u, 14u}, 3u) == 10u,
          "local actor layout resolves selected model from native weapon class and item record");
    check(weapon::effective_model_for_actor(ram, local_actor, transmog::WeaponAppearance{2u, 5u}, 3u) == 9u,
          "local actor class mismatch resolves the native model and preserves armor-only root resource");
    check(weapon::effective_model_for_actor(ram, remote_actor, transmog::WeaponAppearance{2u, 15u}, 3u) == 25u,
          "remote actor layout resolves selected model from its inline weapon record");
    check(weapon::effective_model_for_actor(ram, remote_actor, std::nullopt, 3u) == 20u,
          "unselected weapon resolves to its native model for cached-root comparison");
    check(weapon::effective_model_for_actor(ram, local_actor, transmog::WeaponAppearance{9u, 14u}, 3u) == 9u,
          "invalid matching-class donor resolves to native stock model for cached-root comparison");
    check(weapon::effective_resource_id_for_actor(ram, local_actor, transmog::WeaponAppearance{2u, 14u}, 3u) == 0x06ECu,
          "local model 10 maps through resource-category 9 base 0x6E2 to cached resource 0x6EC");
    check(weapon::effective_resource_id_for_actor(ram, local_actor, transmog::WeaponAppearance{2u, 5u}, 3u) == 0x06EBu,
          "local class mismatch maps the native model through the same gender resource table");
    check(weapon::effective_resource_id_for_actor(ram, remote_actor, transmog::WeaponAppearance{2u, 15u}, 3u) == 0x06FBu,
          "remote model 25 maps through resource-category 9 base 0x6E2");
    expect_error([&] { (void)weapon::select_appearance(ram, transmog::WeaponAppearance{1u, 4u}, 3u, 4u); },
                 "armor kind is not accepted as a weapon class");
    for (const auto id : {0u, 3u, 65535u})
        expect_error([&] { (void)weapon::select_appearance(ram, transmog::WeaponAppearance{static_cast<std::uint16_t>(id), 5u}, 3u, 5u); },
                     "empty or out-of-catalog weapon donor is rejected");
    expect_error([&] { (void)weapon::select_appearance(ram, transmog::WeaponAppearance{1u, 5u}, 0u, 5u); },
                 "unloaded weapon names cannot validate a donor");
    expect_error([&] { (void)weapon::select_appearance(ram, transmog::WeaponAppearance{1u, 5u}, 4097u, 5u); },
                 "implausible weapon catalog is rejected");
    BufferRam missing(0x08800000u, 64u);
    expect_error([&] { (void)weapon::select_appearance(missing, transmog::WeaponAppearance{1u, 5u}, 3u, 5u); },
                 "missing model memory is rejected before reading");
    check(records(ram) == cached_root_baseline, "resource-ID resolution preserves all native weapon model records");
    // The real host hook's package gate and register preservation, using a
    // synthetic original function and a private temporary appearance store.
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("yakumo-weapon-hook-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    transmog::Store store;
    store.initialize(root / "transmog.ini");
    store.set(0u, transmog::ArmorAppearance{1u, 1u});
    store.set_weapon(transmog::WeaponAppearance{1u, 5u});
    const auto choices = store.active();
    fixture_store = &store;
    psprecomp::Runtime runtime(64u * 1024u * 1024u);
    constexpr auto helper = 0x0886A568u;
    constexpr auto caller = 0x088F4738u;
    constexpr auto record = 0x09A00000u;
    runtime.register_function(helper, &original, "test_weapon_original");
    runtime.register_function(0x0886A534u, &original, "test_local_weapon_original");
    for (const auto &word : std::array<transmog::CodeWord, 11>{{
             {0x0886A568u, 0x94A60002u}, {0x0886A56Cu, 0x90A50001u}, {0x0886A570u, 0x0A21A91Fu},
             {0x088F4730u, 0x0E21A95Au}, {0x088F4734u, 0u},
             {0x0886A534u, 0x00051200u}, {0x0886A538u, 0x00052880u},
             {0x0886A53Cu, 0x00451023u}, {0x0886A550u, 0x0A21A91Fu},
             {0x088A59C0u, 0x0E21A94Du}, {0x088A59C4u, 0u}}})
        runtime.memory().store32(word.address, word.word);
    runtime.memory().store8(record, 1u);
    runtime.memory().store8(record + 1u, 5u);
    runtime.memory().store16(record + 2u, 2u);
    runtime.memory().store16(0x08997AA0u + 0x1Cu, 313u);
    runtime.memory().store32(0x0896FBC8u + 0xA8u, 0x088A58DCu);
    runtime.memory().store32(0x0896FED8u + 0xA8u, 0x088A58DCu);
    weapon::install(runtime, &original, store, &feature, {&names, &count}, &selection,
        "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c");
    constexpr std::uint32_t registry_pointer = 0x09FBE794u;
    constexpr std::uint32_t records_pointer = 0x08AB3640u;
    constexpr std::uint32_t registry = 0x09B00000u;
    constexpr std::uint32_t actor = 0x09B01000u;
    constexpr std::uint32_t records_base = 0x09A00000u;
    constexpr std::uint32_t hunter_identity = 3u;
    constexpr std::uint32_t hunter_record = records_base + hunter_identity * 252u + 48u;
    constexpr std::uint32_t own_record = records_base + 48u;
    runtime.memory().store32(registry_pointer, registry);
    runtime.memory().store32(registry + 8u, actor);
    runtime.memory().store32(records_pointer, records_base);
    runtime.memory().store32(actor, 0x0896FBC8u);
    runtime.memory().store32(actor + 4u, 1u);
    runtime.memory().store16(actor + 96u, hunter_identity);
    runtime.memory().store32(actor + 2944u, 4u);
    runtime.memory().store8(actor + 5154u, 1u);
    runtime.memory().store16(hunter_record + 78u, 2u);
    runtime.memory().store8(hunter_record + 118u, 15u);
    runtime.memory().store16(own_record + 78u, 2u);
    runtime.memory().store8(own_record + 118u, 5u);
    constexpr std::uint32_t registry_remote_actor = 0x09B02000u;
    runtime.memory().store32(registry + 12u, registry_remote_actor);
    runtime.memory().store32(registry_remote_actor, 0x0896FED8u);
    runtime.memory().store32(registry_remote_actor + 4u, 1u);
    runtime.memory().store16(registry_remote_actor + 96u, 1u);
    runtime.memory().store32(registry_remote_actor + 2944u, 4u);
    runtime.memory().store8(registry_remote_actor + 5154u, 1u);
    runtime.memory().store16(registry_remote_actor + 1162u, 2u);
    runtime.memory().store8(registry_remote_actor + 1161u, 15u);
    runtime.memory().store16(actor + 96u, 0u);
    check(weapon::equipped_weapon_kind() == 5u,
          "the verified record-zero FBC8 owner reads its weapon class from the native hunter table");
    runtime.memory().store16(actor + 96u, hunter_identity);
    runtime.memory().store16(registry_remote_actor + 96u, 0u);
    runtime.memory().store8(registry_remote_actor + 1161u, 12u);
    check(weapon::equipped_weapon_kind() == 12u,
          "record-zero FED8 file-function owner is selected over a ready non-owner FBC8 with another class");
    runtime.memory().store8(registry_remote_actor + 1161u, 5u);
    check(weapon::equipped_weapon_kind() == 5u,
          "the local player's native inline weapon record remains authoritative when another hunter is ready");
    runtime.memory().store16(actor + 96u, 6u);
    check(weapon::equipped_weapon_kind() == 5u,
          "a ready non-owner with an out-of-range record index is ignored rather than mistaken for the local hunter");
    runtime.memory().store16(registry_remote_actor + 1162u, 0u);
    check(!weapon::equipped_weapon_kind(), "an empty local weapon reports no equipped class even when another hunter is armed");
    runtime.memory().store16(registry_remote_actor + 1162u, 2u);
    runtime.memory().store32(registry + 12u, 0u);
    check(!weapon::equipped_weapon_kind(), "ready non-owner hunters alone do not report the local player's weapon class");
    runtime.memory().store32(registry + 12u, registry_remote_actor);
    runtime.memory().store16(actor + 96u, hunter_identity);
    const auto invoke = [&](std::uint32_t return_address) {
        psprecomp::AllegrexContext ctx{};
        ctx.gpr[5] = record;
        ctx.gpr[31] = return_address;
        ctx.gpr[16] = 456u;
        ctx.lo = 789u;
        check(runtime.invoke_isolated_aot(helper, ctx), "weapon hook dispatches");
        check(ctx.pc == return_address && ctx.gpr[9] == 91u && ctx.hi == 123u &&
                  ctx.gpr[16] == 456u && ctx.lo == 789u &&
                  ctx.gpr[5] == runtime.memory().load8(record + 1u) && ctx.gpr[6] == 2u,
              "original register effects and preserved ABI state survive the hook");
        return ctx.gpr[2];
    };
    feature_enabled = false;
    check(invoke(caller) == 71u, "disabled runtime gate delegates the original model");
    feature_enabled = true;
    check(invoke(caller) == 313u, "matching class and verified visual caller use donor model");
    check(invoke(caller + 4u) == 71u, "nearby unverified caller keeps original model");
    const auto invoke_local = [&](std::uint8_t kind, std::uint32_t return_address) {
        psprecomp::AllegrexContext ctx{};
        ctx.pc = 0x0886A47Cu;
        ctx.gpr[5] = kind;
        ctx.gpr[6] = 2u;
        ctx.gpr[31] = return_address;
        check(runtime.invoke_isolated_aot(0x0886A47Cu, ctx), "local visual weapon hook dispatches");
        check(ctx.pc == return_address && ctx.gpr[9] == 91u && ctx.hi == 123u,
              "local visual weapon hook preserves original ABI effects");
        return ctx.gpr[2];
    };
    check(invoke_local(5u, 0x088A59C8u) == 313u,
          "local visual resource caller uses donor model for the matching weapon class");
    check(invoke_local(12u, 0x088A59C8u) == 71u && store.active() == choices,
          "local class mismatch suppresses only the weapon appearance and keeps armor and preset binding");
    check(invoke_local(5u, 0x088A59C8u) == 313u,
          "local saved weapon appearance resumes when its class returns");
    check(invoke_local(5u, 0x088A59CCu) == 71u,
          "nearby local weapon return sites keep the original model");
    const auto invoke_local_entry = [&](std::uint8_t kind, std::uint32_t return_address) {
        psprecomp::AllegrexContext ctx{};
        ctx.pc = 0x0886A534u;
        ctx.gpr[4] = records_base;
        ctx.gpr[5] = hunter_identity;
        ctx.gpr[31] = return_address;
        runtime.memory().store8(hunter_record + 118u, kind);
        check(runtime.invoke_isolated_aot(0x0886A534u, ctx), "local weapon record entry dispatches");
        check(ctx.pc == return_address && ctx.gpr[5] == kind && ctx.gpr[6] == 2u &&
                  ctx.gpr[9] == 91u && ctx.hi == 123u,
              "local record entry preserves native decode, caller and ABI effects");
        return ctx.gpr[2];
    };
    check(invoke_local_entry(5u, 0x088A59C8u) == 313u,
          "local resource builder entry uses same-class donor model after native record decode");
    check(invoke_local_entry(12u, 0x088A59C8u) == 71u && store.active() == choices,
          "local resource builder retains stock model on class mismatch without changing saved choices");
    check(invoke_local_entry(5u, 0x088A59CCu) == 71u,
          "local record entry keeps native model for an unverified return site");
    runtime.memory().store8(record + 1u, 12u);
    check(invoke(caller) == 71u, "changing class suppresses only the weapon cosmetic");
    check(store.active() == choices, "runtime class change keeps armor and saved weapon choices intact");
    runtime.memory().store8(record + 1u, 5u);
    check(invoke(caller) == 313u, "returning to original class activates saved cosmetic again");
    store.set_weapon(transmog::WeaponAppearance{3u, 5u});
    check(invoke(caller) == 71u && store.active().armor == choices.armor &&
          weapon::status().find("outside its loaded catalog") != std::string::npos,
          "invalid persisted weapon donor keeps stock weapon, armor choices and a visible recovery diagnosis");
    store.set_weapon(choices.weapon);
    check(invoke(caller) == 313u, "correcting a saved weapon donor recovers without restarting");
    rebuilding = store.active();
    store.set_weapon(std::nullopt);
    check(invoke(caller) == 313u && !store.active().weapon,
          "in-flight visual rebuild keeps its immutable choice when live preferences change");
    rebuilding.reset();
    check(invoke(caller) == 71u && store.active().armor == choices.armor,
          "weapon Default restores stock appearance while preserving armor");
    check(runtime.memory().load8(record) == 1u && runtime.memory().load8(record + 1u) == 5u &&
              runtime.memory().load16(record + 2u) == 2u && runtime.memory().load16(0x08997AA0u + 0x1Cu) == 313u,
          "hook never changes equipped record or model table");
    check(name_reads == 0u, "native model selection reads catalog counts without allocating names");
    fs::remove_all(root);
    std::cout << "Transmog weapons: " << failures << " failure(s)\n";
    return failures == 0u ? 0 : 1;
}
