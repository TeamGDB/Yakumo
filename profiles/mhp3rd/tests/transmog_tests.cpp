// Synthetic armor model records: verifies cosmetic lookup without game data.
#include "mods/transmog.hpp"
#include "game/guest_ram.hpp"
#include "settings/settings.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <string_view>
#include <vector>

namespace mhp3rd::install {
void set_transmog_test_data_directory(std::filesystem::path path);
}

namespace {
namespace transmog = mhp3rd::mods::transmog;
using mhp3rd::game::BufferRam;
unsigned failures{};
constexpr std::array<std::uint32_t, 5> tables{
    0x08987EE4u, 0x08985A7Cu, 0x08983934u, 0x0898A6E4u, 0x0898C854u};
constexpr std::array<std::uint32_t, 5> kinds{4u, 0u, 1u, 2u, 3u};
constexpr std::uint32_t kText = 0x08A40640u;
constexpr std::array<std::uint32_t, 5> kNameTables{29u, 31u, 33u, 35u, 37u};
constexpr std::string_view kSupportedExecutable = "55c0598436c0753b04331f8e95d406f832d9217806e3a896fed0e88b33637d8c";

void check(bool success, std::string_view message) {
    if (!success) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
void original_appearance(psprecomp::Runtime &, psprecomp::AllegrexContext &) {}

void prepare_runtime(psprecomp::Runtime &runtime) {
    for (const auto &word : transmog::helper_signature()) runtime.memory().store32(word.address, word.word);
    runtime.register_function(0x08869564u, &original_appearance, "test_original_model");
    runtime.register_function(0x088A51E4u, &original_appearance, "test_original_converter");
    runtime.register_function(0x08869778u, &original_appearance, "test_local_model");
    runtime.register_function(0x088A58DCu, &original_appearance, "test_local_resource_resolver");

    // A loaded synthetic hunter and five loaded two-entry armor name tables
    // allow the production migration path to run without game assets.
    runtime.memory().store16(0x09F4FCACu, 1u);
    runtime.memory().store32(0x08AB3640u, 0x08820000u);
    runtime.memory().store8(0x08820877u, 1u);
    for (std::size_t slot = 0; slot < kNameTables.size(); ++slot) {
        const auto directory = 0x1000u + static_cast<std::uint32_t>(slot) * 0x100u;
        const auto table = kText + directory;
        runtime.memory().store32(kText + kNameTables[slot] * 4u, directory);
        runtime.memory().store32(table, 12u); // two names and the terminal marker
        runtime.memory().store32(table + 4u, 12u);
        runtime.memory().store32(table + 8u, 0xFFFFFFFFu);
        runtime.memory().store16(tables[slot] + 0x28u, static_cast<std::uint16_t>(100u + slot));
        runtime.memory().store16(tables[slot] + 0x2Au, static_cast<std::uint16_t>(200u + slot));
    }
}

int run_migration_case(std::string_view mode) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("yakumo-transmog-migration-" + std::string(mode));
    fs::remove_all(root);
    fs::create_directories(root);
    mhp3rd::install::set_transmog_test_data_directory(root);
    auto &legacy_settings = mhp3rd::settings::current();
    legacy_settings.layered_armor = true;
    legacy_settings.layered_pieces = {1, 0, -1, 1, 0};

    const auto config = root / "transmog.ini";
    const std::string valid =
        "[transmog]\nversion=2\n\n[active]\n"
        "Head=1:0\nChest=2:1\nArms=default\nWaist=1:0\nLegs=default\nWeapon=default\n";
    if (mode == "precedence") {
        std::ofstream output(config, std::ios::binary);
        output << valid;
    } else if (mode == "invalid") {
        std::ofstream output(config, std::ios::binary);
        output << "[transmog]\nversion=2\n\n[active]\nHead=not-an-appearance\n";
    }
    std::string original_file;
    if (fs::exists(config)) {
        std::ifstream input(config, std::ios::binary);
        original_file.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    psprecomp::Runtime runtime;
    prepare_runtime(runtime);
    transmog::install(runtime, {&original_appearance, &original_appearance}, kSupportedExecutable);
    const auto &selection = transmog::current_selection();
    if (mode == "migration") {
        check(selection.armor[0] == transmog::ArmorAppearance{0u, 1u} &&
              selection.armor[1] == transmog::ArmorAppearance{1u, 1u} &&
              selection.armor[2] == transmog::ArmorAppearance{0u, 1u} &&
              !selection.armor[3] && selection.armor[4] == transmog::ArmorAppearance{1u, 1u},
              "legacy chest/arms/waist/legs/head values migrate into head/chest/arms/waist/legs slots using hunter gender");
        transmog::Store persisted;
        persisted.initialize(config);
        check(persisted.active() == selection, "legacy import is persisted into transmog.ini");
        legacy_settings.layered_pieces = {0, 1, 0, -1, 1};
        check(transmog::current_selection() == selection,
              "legacy layered values are ignored after the one-time import");
    } else if (mode == "precedence") {
        transmog::Set expected{};
        expected.armor[0] = transmog::ArmorAppearance{1u, 0u};
        expected.armor[1] = transmog::ArmorAppearance{2u, 1u};
        expected.armor[3] = transmog::ArmorAppearance{1u, 0u};
        check(selection == expected, "valid transmog.ini takes precedence over legacy layered pieces");
        std::ifstream input(config, std::ios::binary);
        const std::string after{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        check(after == original_file, "precedence check does not rewrite the valid store from legacy values");
    } else if (mode == "invalid") {
        check(!transmog::configuration_problem().empty(), "invalid transmog.ini produces a visible configuration problem");
        check(selection == transmog::Set{}, "invalid store prevents legacy migration from becoming active");
        std::ifstream input(config, std::ios::binary);
        const std::string after{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        check(after == original_file, "invalid store remains intact until explicit recovery");
    }
    fs::remove_all(root);
    return failures == 0u ? 0 : 1;
}

void run_migration_processes(const char *executable) {
    for (const char *mode : {"migration", "precedence", "invalid"}) {
        std::ostringstream command;
        command << std::quoted(executable) << " --transmog-case " << mode;
        check(std::system(command.str().c_str()) == 0, std::string("isolated migration case passes: ") + mode);
    }
}
template<class F> void expect_error(F operation, std::string_view message) {
    try { operation(); check(false, message); }
    catch (const psprecomp::Error &error) {
        check(std::string_view(error.what()).size() > 20u, "failure includes diagnosis");
    }
}
std::vector<std::uint8_t> records(const BufferRam &ram) {
    std::vector<std::uint8_t> bytes;
    for (auto table : tables)
        for (unsigned offset = 0; offset < 3u * 0x28u; ++offset) bytes.push_back(ram.load8(table + offset));
    return bytes;
}
}

int main(int argc, char **argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--transmog-case") {
        try {
            return run_migration_case(argv[2]);
        } catch (const std::exception &error) {
            std::cerr << "FAIL: unexpected exception in transmog case '" << argv[2] << "': " << error.what() << '\n';
            return 1;
        }
    }
    run_migration_processes(argv[0]);

    BufferRam ram(0x08800000u, 0x00200000u);
    const std::array<std::size_t, 5> counts{3u, 3u, 3u, 3u, 3u};
    for (std::size_t slot = 0; slot < tables.size(); ++slot) {
        for (unsigned id = 0; id < 3u; ++id) {
            const auto address = tables[slot] + id * 0x28u;
            for (unsigned offset = 4u; offset < 0x28u; ++offset)
                ram.store8(address + offset, static_cast<std::uint8_t>(slot + id + offset));
            ram.store16(address, id == 0u ? 0u : static_cast<std::uint16_t>(100u + slot * 10u + id));
            ram.store16(address + 2u, id == 0u ? 0u : static_cast<std::uint16_t>(200u + slot * 10u + id));
        }
    }
    const auto baseline = records(ram);
    transmog::Set selected{};
    for (std::size_t slot = 0; slot < tables.size(); ++slot) {
        check(!transmog::select_appearance(ram, selected, counts, kinds[slot], 0u),
              "default delegates equipped armor appearance");
        selected.armor[slot] = transmog::ArmorAppearance{1u, 0u};
        check(transmog::select_appearance(ram, selected, counts, kinds[slot], 0u) == 101u + slot * 10u,
              "slot maps to its male donor model");
        check(transmog::select_appearance(ram, selected, counts, kinds[slot], 1u) == 101u + slot * 10u,
              "a female hunter can select the male donor model");
        selected.armor[slot] = transmog::ArmorAppearance{1u, 1u};
        check(transmog::select_appearance(ram, selected, counts, kinds[slot], 0u) == 201u + slot * 10u,
              "a male hunter can select the female donor model");
        selected.armor[slot] = transmog::ArmorAppearance{0u, 0u};
        const auto bare = transmog::select_appearance(ram, selected, counts, kinds[slot], 0u);
        check(bare.has_value() && *bare == 0u, "bare armor zero remains distinct from Default");
        selected.armor[slot].reset();
    }
    check(!transmog::select_appearance(ram, selected, counts, 5u, 0u), "weapons keep original lookup");
    selected.armor[0] = transmog::ArmorAppearance{1u, 2u};
    expect_error([&] { (void)transmog::select_appearance(ram, selected, counts, 4u, 0u); },
                 "unsupported donor gender is rejected");
    selected.armor[0] = transmog::ArmorAppearance{3u, 0u};
    expect_error([&] { (void)transmog::select_appearance(ram, selected, counts, 4u, 0u); },
                 "donor at catalog end is rejected");
    selected.armor[0] = transmog::ArmorAppearance{65535u, 0u};
    expect_error([&] { (void)transmog::select_appearance(ram, selected, counts, 4u, 0u); },
                 "largest serialized donor cannot access unrelated memory");
    selected.armor[0] = transmog::ArmorAppearance{1u, 0u};
    auto invalid_counts = counts;
    invalid_counts[0] = 0u;
    expect_error([&] { (void)transmog::select_appearance(ram, selected, invalid_counts, 4u, 0u); },
                 "unloaded catalog is rejected");
    invalid_counts[0] = 4097u;
    expect_error([&] { (void)transmog::select_appearance(ram, selected, invalid_counts, 4u, 0u); },
                 "implausible catalog length is rejected");
    BufferRam missing(0x08800000u, 64u);
    expect_error([&] { (void)transmog::select_appearance(missing, selected, counts, 4u, 0u); },
                 "out-of-memory model record is rejected before reading");
    check(records(ram) == baseline, "lookups and errors preserve all model, stats and skills bytes");
    // A missing donor model is invalid independently of the hunter's gender.
    ram.store16(tables[0] + 0x28u + 2u, 0u);
    const auto gender_baseline = records(ram);
    selected.armor[0] = transmog::ArmorAppearance{1u, 1u};
    expect_error([&] { (void)transmog::select_appearance(ram, selected, counts, 4u, 0u); },
                 "donor without its selected source-gender model is rejected");
    selected.armor[0] = transmog::ArmorAppearance{1u, 0u};
    check(transmog::select_appearance(ram, selected, counts, 4u, 0u) == 101u,
          "the same armor id remains valid with its existing source model");
    selected = {};
    for (auto kind : kinds)
        check(!transmog::select_appearance(ram, selected, counts, kind, 0u), "reset restores every default lookup");
    check(records(ram) == gender_baseline, "reset and compatibility checks preserve original records");
    // Synthetic male/female resource bases exercise the native continuation:
    // it adds the actor's saved base after the converter hook returns.
    for (const auto donor_base : {0x0100u, 0x5000u})
        for (const auto converted : {0u, 17u, 65535u})
            for (const auto actor_base : {0u, 0x1000u, 0xFFF0u}) {
                const auto adjusted = transmog::converter_adjustment(
                    static_cast<std::uint16_t>(donor_base), static_cast<std::uint16_t>(converted),
                    static_cast<std::uint16_t>(actor_base));
                const auto native_result = (adjusted + actor_base) & 0xFFFFu;
                check(native_result == ((donor_base + converted) & 0xFFFFu),
                      "native continuation produces donor-sex asset id across 16-bit wrap");
            }
    check(transmog::converter_adjustment(1u, 2u, 65535u) == 4u,
          "subtracting a greater actor base wraps to the required unsigned correction");
    check(records(ram) == gender_baseline, "resource correction never changes armor records");

    namespace fs = std::filesystem;
    const auto reset_root = fs::temp_directory_path() / ("yakumo-transmog-preview-reset-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(reset_root);
    auto &store = transmog::store();
    store.initialize(reset_root / "transmog.ini");
    transmog::Set saved{};
    saved.armor[0] = transmog::ArmorAppearance{17u, 1u};
    saved.armor[3] = transmog::ArmorAppearance{8u, 0u};
    saved.weapon = transmog::WeaponAppearance{5u, 14u};
    store.apply(saved);
    store.save_preset("Saved outfit");
    const auto saved_presets = store.presets();

    transmog::begin_preview();
    transmog::reset_appearances();
    transmog::reset_appearances();
    check(transmog::preview_active() && transmog::current_selection() == transmog::Set{},
          "reset during preview stages default armor and weapon choices and tolerates repetition");
    check(store.active() == saved && store.presets() == saved_presets,
          "preview reset leaves saved active appearances and named sets untouched");
    transmog::cancel_preview();
    transmog::cancel_preview();
    check(!transmog::preview_active() && store.active() == saved && transmog::current_selection() == saved,
          "Cancel and repeated close restore the saved active set after preview reset");
    transmog::Store after_cancel;
    after_cancel.initialize(reset_root / "transmog.ini");
    check(after_cancel.active() == saved && after_cancel.presets() == saved_presets,
          "preview reset followed by Cancel preserves active set and presets on disk");

    transmog::begin_preview();
    transmog::reset_appearances();
    transmog::apply_preview();
    check(!transmog::preview_active() && store.active() == transmog::Set{},
          "Apply commits previewed defaults and closes the preview");
    check(store.presets() == saved_presets,
          "Apply after preview reset retains all named saved sets");
    transmog::Store after_apply;
    after_apply.initialize(reset_root / "transmog.ini");
    check(after_apply.active() == transmog::Set{} && after_apply.presets() == saved_presets,
          "preview reset Apply persists defaults and named sets");

    store.apply(saved);
    transmog::reset_appearances();
    transmog::reset_appearances();
    check(!transmog::preview_active() && store.active() == transmog::Set{} && store.presets() == saved_presets,
          "reset outside preview remains immediate, repeatable and preset-preserving");
    fs::remove_all(reset_root);
    std::cout << "Transmog selector: " << failures << " failure(s)\n";
    return failures == 0u ? 0 : 1;
}
