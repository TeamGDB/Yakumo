#include "mods/transmog_store.hpp"
#include "psprecomp/common.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
int failures{};
void check(bool success, const char *message) {
    if (!success) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class F> void expect_error(F operation, const char *message) {
    try { operation(); check(false, message); }
    catch (const psprecomp::Error &error) { check(std::string_view(error.what()).size() > 20u, "error supplies context"); }
}
}

int main() {
    namespace fs = std::filesystem;
    using mhp3rd::mods::transmog::Store;
    using mhp3rd::mods::transmog::ArmorAppearance;
    using mhp3rd::mods::transmog::WeaponAppearance;
    const auto root = fs::temp_directory_path() / ("yakumo-transmog-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const auto path = root / "transmog.ini";
    Store store;
    expect_error([&] { store.reset(); }, "uninitialized writes rejected");
    store.initialize(path);
    check(store.active() == mhp3rd::mods::transmog::Set{} && store.presets().empty(), "missing config starts at default");
    store.set(0, ArmorAppearance{7, 0});
    store.set(1, ArmorAppearance{0, 1});
    store.set(4, ArmorAppearance{27, 1});
    store.set_weapon(WeaponAppearance{19, 14});
    store.save_preset("Village outfit");
    const auto outfit = store.active();
    store.set(2, ArmorAppearance{35, 0});
    store.save_preset("Hunting");
    const auto hunting = store.active();
    store.reset();
    check(store.active() == mhp3rd::mods::transmog::Set{}, "reset clears every active override");
    check(store.presets().size() == 2u, "reset retains saved sets");
    Store loaded;
    loaded.initialize(path);
    check(loaded.active() == store.active() && loaded.presets() == store.presets(), "active default and presets survive restart");
    loaded.load_preset("Village outfit");
    check(loaded.active() == outfit, "load applies armor slots, donor gender and same-class weapon binding");
    loaded.load_preset("Hunting");
    check(loaded.active() == hunting, "second saved set loads independently");
    loaded.save_preset("Village outfit");
    check(loaded.presets().at("Village outfit") == hunting, "save existing name overwrites that preset");
    loaded.delete_preset("Hunting");
    check(loaded.active() == hunting && loaded.presets().size() == 1u, "delete removes only the named preset");
    auto draft = loaded.active();
    draft.armor[0] = ArmorAppearance{42, 1};
    loaded.save_preset("Preview draft", draft);
    check(loaded.active() == hunting && loaded.presets().at("Preview draft") == draft,
          "saving a preview draft leaves the active look unchanged");
    loaded.apply(draft);
    check(loaded.active() == draft && loaded.presets().at("Village outfit") == hunting,
          "applying a preview replaces only the active set and retains saved presets");
    expect_error([&] { loaded.set(5, ArmorAppearance{4, 0}); }, "out of range slot rejected");
    expect_error([&] { loaded.set(0, ArmorAppearance{4, 2}); }, "invalid donor gender rejected");
    expect_error([&] { loaded.set_weapon(WeaponAppearance{4, 10}); }, "unknown weapon class rejected");
    const auto armor_before_weapon = loaded.active().armor;
    loaded.set_weapon(WeaponAppearance{3, 5});
    check(loaded.active().armor == armor_before_weapon, "changing weapon appearance preserves every armor choice");
    loaded.set_weapon(std::nullopt);
    check(loaded.active().armor == armor_before_weapon, "resetting weapon appearance preserves every armor choice");
    expect_error([&] { loaded.save_preset("bad\nname"); }, "unsafe preset name rejected");
    expect_error([&] { loaded.save_preset(" empty "); }, "ambiguous surrounding whitespace rejected");
    expect_error([&] { loaded.load_preset("missing"); }, "missing set load rejected");
    expect_error([&] { loaded.delete_preset("missing"); }, "missing set delete rejected");
    const auto before = loaded.active();
    const auto partial = fs::path(path.native() + fs::path(".part").native());
    fs::create_directory(partial);
    expect_error([&] { loaded.reset(); }, "write failure surfaces");
    check(loaded.active() == before, "failed write leaves active appearances unchanged");
    fs::remove(partial);
    Store persisted;
    persisted.initialize(path);
    check(persisted.active() == before, "failed write preserves old disk config");
    const std::string valid_file = [&] {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    std::string invalid_file = valid_file;
    const auto head = invalid_file.find("Head=");
    check(head != std::string::npos, "test config contains Head field");
    if (head != std::string::npos) {
        const auto end = invalid_file.find('\n', head);
        invalid_file.replace(head, end == std::string::npos ? invalid_file.size() - head : end - head, "Head=garbage");
    }
    { std::ofstream invalid(path, std::ios::binary); invalid << invalid_file; }
    expect_error([&] { persisted.initialize(path); }, "malformed armor id rejected");
    check(persisted.active() == before, "invalid reload preserves previous valid state");
    check(persisted.path() == path, "invalid reload retains path for explicit recovery");
    expect_error([&] { persisted.reset(); }, "invalid settings cannot be overwritten implicitly");
    const auto occupied_backup = fs::path(path.native() + fs::path(".invalid-backup-0").native());
    { std::ofstream occupied(occupied_backup); occupied << "preserve existing backup"; }
    const auto backup = persisted.back_up_invalid_and_reset();
    check(backup == fs::path(path.native() + fs::path(".invalid-backup-1").native()), "recovery chooses a unique backup name");
    {
        std::ifstream preserved(backup, std::ios::binary);
        check(std::string(std::istreambuf_iterator<char>(preserved), std::istreambuf_iterator<char>()) == invalid_file,
              "recovery preserves the malformed file and its saved sets");
    }
    {
        std::ifstream occupied(occupied_backup, std::ios::binary);
        check(std::string(std::istreambuf_iterator<char>(occupied), std::istreambuf_iterator<char>()) == "preserve existing backup",
              "recovery does not overwrite a previous backup");
    }
    Store recovered;
    recovered.initialize(path);
    check(recovered.active() == mhp3rd::mods::transmog::Set{} && recovered.presets().empty(),
          "explicit invalid-file recovery creates a valid default store");
    Store rename_failure;
    const auto blocked = root / "blocked.ini";
    rename_failure.initialize(blocked);
    auto load_target = rename_failure.active();
    load_target.armor[0] = ArmorAppearance{7, 0};
    rename_failure.save_preset("Load target", load_target);
    fs::remove(blocked);
    fs::create_directory(blocked);
    try {
        rename_failure.load_preset("Load target");
        check(false, "preset load rename failure surfaces");
    } catch (const fs::filesystem_error &error) {
        check(std::string_view(error.what()).find("rename") != std::string_view::npos, "rename error identifies failing operation");
    }
    check(rename_failure.active() == mhp3rd::mods::transmog::Set{}, "failed preset load leaves active appearances unchanged");
    check(rename_failure.presets().at("Load target") == load_target, "failed preset load preserves its named set");
    check(fs::is_directory(blocked), "failed replacement preserves destination");
    fs::remove_all(root);
    std::cout << "Transmog store: " << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
