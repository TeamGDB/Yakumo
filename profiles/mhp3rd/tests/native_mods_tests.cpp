// Native mods through the real loader: the test mod (native_test_mod.cpp)
// is loaded from its shared library, takes hooks on synthetic guest
// functions, and logs what it sees; the guest code runs through the JIT.
#include "mods/mod_library.hpp"
#include "mods/mhp3rd_added_files.hpp"
#include "mods/mhp3rd_equipment.hpp"
#include "mods/mhp3rd_maps.hpp"
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mhp3rd_monsters.hpp"
#include "mods/mhp3rd_pictures.hpp"
#include "mods/mod_native.hpp"
#include "mods/mhp3rd_quests.hpp"
#include "mods/mhp3rd_symbols.hpp"
#include "mods/mhp3rd_tables.hpp"

#include "yakumo/mod.hpp"

#include "game/game_data.hpp"
#include "game/guest_ram.hpp"
#include "game/pack_file.hpp"
#include "game/quest_builder.hpp"
#include "game/quest_file.hpp"
#include "kernel/kernel.hpp"

#include "psprecomp/jit.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <map>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

int failures = 0;

void check(bool condition, const std::string &what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) ++failures;
}

constexpr std::uint32_t kSentinel = 0x08800000u;

void done(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &) {
    runtime.stop("returned");
}

bool logged(const std::string &text) {
    for (const auto &report : mhp3rd::mods::native::reports())
        if (report.line == text) return true;
    return false;
}

// A line that starts with `start`, as a problem: for messages that end with
// what the platform says.
bool logged_starting(const std::string &start) {
    for (const auto &report : mhp3rd::mods::native::reports())
        if (report.line.starts_with(start) && report.line.size() > start.size() && report.problem) return true;
    return false;
}

// On the page of the mod named `mod`, as a problem.
bool reported(const std::string &mod, const std::string &text) {
    for (const auto &report : mhp3rd::mods::native::reports())
        if (report.mod == mod && report.line == text && report.problem) return true;
    return false;
}

void store(psprecomp::Runtime &runtime, std::uint32_t address, std::initializer_list<std::uint32_t> words) {
    for (const std::uint32_t word : words) {
        runtime.memory().store32(address, word);
        address += 4u;
    }
}

// Calls the guest function at `address` with a0 and returns v0.
std::uint32_t call(psprecomp::Runtime &runtime, std::uint32_t address, std::uint32_t a0) {
    runtime.cpu().gpr[4] = a0;
    runtime.cpu().gpr[31] = kSentinel;
    std::ostringstream quiet;
    std::streambuf *previous = std::cerr.rdbuf(quiet.rdbuf());
    runtime.run(address, 10000u);
    std::cerr.rdbuf(previous);
    return runtime.cpu().gpr[2];
}

// A host feature replacing the hooked function: twice the argument.
void host_feature(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ctx.gpr[2] = ctx.gpr[4] * 2u;
    ctx.pc = ctx.gpr[31];
}

// Files mods add, served through stand-ins for the game's loader functions
// that begin as the traced ones do.
void loader_stand_ins(psprecomp::Runtime &runtime) {
    // size(loader, id): 0x777 for the game's files.
    store(runtime, 0x08864E8Cu,
        {0x30A5FFFFu, 0x3402FFFFu, 0x00001821u, 0x10A20010u, 0u, 0x00801821u, 0x03E00008u, 0x24020777u});
    store(runtime, 0x08864EDCu, {0x03E00008u, 0u});
    // submit(loader, id, destination): 2 for the game's files.
    store(runtime, 0x08863CDCu,
        {0x27BDFFD0u, 0u, 0x00808021u, 0u, 0u, 0x313700FFu, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0x30B2FFFFu, 0x24020002u,
            0x03E00008u, 0x27BD0030u});
    // read(loader, id, destination, length): 3 for the game's files.
    store(runtime, 0x08864C54u, {0x27BDFFE0u, 0x27BD0020u, 0x03E00008u, 0x24020003u});
    store(runtime, 0x08864C6Cu, {0x30B5FFFFu});
    store(runtime, 0x08864CFCu, {0x8C620014u});
}

void test_added_files(psprecomp::Runtime &runtime) {
    namespace added = mhp3rd::mods::added;
    loader_stand_ins(runtime);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "yakumo_added_file_test.bin";
    std::ofstream(file, std::ios::binary) << "ADDED FILE";
    const auto run = [&](std::uint32_t address, std::uint32_t id, std::uint32_t destination) {
        runtime.cpu().gpr[5] = id;
        runtime.cpu().gpr[6] = destination;
        return call(runtime, address, 0x08B3F040u);
    };

    check(added::install(runtime, {}) && !added::serves(0x2000u), "added: nothing to serve installs nothing");
    check(run(0x08864E8Cu, 5u, 0u) == 0x777u, "added: the stand-in loader answers before anything is added");
    runtime.memory().store32(0x08863CF0u, 0u);
    check(!added::install(runtime, {{0x2000u, {"mod", file, {}}}}) && !added::serves(0x2000u),
        "added: a loader that is not as traced is left alone");
    runtime.memory().store32(0x08863CF0u, 0x313700FFu);
    check(added::install(runtime, {{0x2000u, {"mod", file, {}}}, {0x2001u, {"mod", "/nonexistent/file", {}}}}) &&
            added::serves(0x2000u) && !added::serves(0x2001u),
        "added: installed for the files that can be read");
    check(run(0x08864E8Cu, 0x2000u, 0u) == 10u && runtime.cpu().pc == kSentinel,
        "added: the size lookup answers an added file's size");
    check(run(0x08864E8Cu, 0xFFFF0005u, 0u) == 0x777u, "added: the game's own files still get the game's answer");
    check(run(0x08863CDCu, 0x2000u, 0x08B00000u) == 1u && runtime.memory().load32(0x08B00000u) == 0x45444441u,
        "added: a submit copies the added file to its destination and reports it queued");
    check(run(0x08863CDCu, 7u, 0x08B00000u) == 2u, "added: other files go to the game's submit");
    runtime.memory().store32(0x08B00100u, 0u);
    runtime.memory().store32(0x08B00104u, 0u);
    runtime.cpu().gpr[7] = 0xFFFFFFFFu;
    check(run(0x08864C54u, 0x2000u, 0x08B00100u) == 10u && runtime.memory().load32(0x08B00104u) == 0x49462044u,
        "added: the read that waits copies a whole added file and gives its size");
    runtime.memory().store32(0x08B00104u, 0u);
    runtime.cpu().gpr[7] = 4u;
    check(run(0x08864C54u, 0x2000u, 0x08B00100u) == 4u && runtime.memory().load32(0x08B00104u) == 0u,
        "added: and as much of it as asked for");
    runtime.cpu().gpr[7] = 0xFFFFFFFFu;
    check(run(0x08864C54u, 7u, 0x08B00100u) == 3u, "added: other files go to the game's read");

    // Files a mod makes while running, from memory, under ids handed out.
    const mhp3rd::mods::FileId free = added::free_id();
    check(free == added::kLastFreeId, "added: the first free id is the top of the range");
    check(added::add(runtime, free, {'M', 'E', 'M', 'O', 'R', 'Y'}) && added::serves(free) &&
            added::free_id() == added::kLastFreeId - 1u,
        "added: a file from memory is served, and its id is no longer free");
    check(!added::add(runtime, free, {1u}) && !added::add(runtime, free - 1u, {}),
        "added: an id taken, or no bytes, is refused");
    check(run(0x08864E8Cu, free, 0u) == 6u && run(0x08863CDCu, free, 0x08B00200u) == 1u &&
            runtime.memory().load32(0x08B00200u) == 0x4F4D454Du,
        "added: a file from memory is sized and copied like one on disk");
    added::reset_for_tests();
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// A code overlay's header as the game copies it to its slot: "MWo3", its
// load address, code and data sizes, and its name.
void place_overlay(psprecomp::Runtime &runtime, std::uint32_t base, const std::string &name, std::uint32_t size) {
    for (std::uint32_t at = 0u; at < 64u; at += 4u) runtime.memory().store32(base + at, 0u);
    runtime.memory().store32(base, 0x336F574Du);
    runtime.memory().store32(base + 8u, base);
    runtime.memory().store32(base + 12u, size);
    for (std::size_t i = 0; i < name.size(); ++i)
        runtime.memory().store8(base + 32u + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(name[i]));
}

void test_symbols(psprecomp::Runtime &runtime) {
    namespace symbols = mhp3rd::mods::symbols;
    const std::string a = "a";
    check(symbols::fnv1a({reinterpret_cast<const std::uint8_t *>(a.data()), a.size()}) == 0xE40C292Cu,
        "symbols: FNV-1a of \"a\"");

    std::size_t functions = 0u;
    std::size_t executable_functions = 0u;
    bool well_formed = !symbols::all().empty();
    for (const symbols::Symbol &symbol : symbols::all()) {
        if (!symbol.function) continue;
        ++functions;
        if (symbol.overlay.empty()) ++executable_functions;
        well_formed = well_formed && symbol.check && (symbol.address & 3u) == 0u && !symbol.about.empty();
    }
    check(well_formed && functions > 20u, "symbols: the compiled-in table reads, every function with its check");

    std::vector<std::string> problems;
    const std::vector<symbols::Symbol> parsed = symbols::parse("# a comment\n"
                                                               "function one 0x08800100 - 1234abcd The first\n"
                                                               "data two 0x09000001 game_task - A byte\r\n"
                                                               "function one 0x08800200 - 1234abcd Again\n"
                                                               "function odd 0x08800202 - 1234abcd Unaligned\n"
                                                               "function plain 0x08800300 - - No check\n"
                                                               "thing three 0x08800400 - - Unknown kind\n"
                                                               "data far 0x123456789 - - Too long\n",
        &problems);
    check(parsed.size() == 2u && parsed[0].name == "one" && parsed[0].check == 0x1234ABCDu && parsed[0].function &&
            parsed[1].overlay == "game_task" && !parsed[1].check && parsed[1].about == "A byte" &&
            problems.size() == 5u,
        "symbols: a table's lines, and the ones that do not read");

    check(symbols::find("armor.model_of_part", nullptr) != nullptr &&
            symbols::find("armor.model_of_part", nullptr)->address == 0x08869778u &&
            symbols::find("no.such.name", nullptr) == nullptr,
        "symbols: found by name");
    check(symbols::at(0x08864E8Cu) != nullptr && symbols::at(0x08864E8Cu)->name == "loader.size" &&
            symbols::at(0x08864E90u) == nullptr,
        "symbols: found by address");

    // This memory holds no game code, except the stand-in for the loader's size
    // lookup, which begins with the game's own instructions.
    check(symbols::verify(runtime.memory()) == executable_functions - 1u &&
            symbols::find("loader.size", nullptr) != nullptr,
        "symbols: a function whose first bytes are the game's is given out");
    check(symbols::find("armor.model_of_part", nullptr) == nullptr && symbols::at(0x08869778u) == nullptr &&
            symbols::find("hunter.zenny", nullptr) != nullptr,
        "symbols: functions whose bytes differ are not given out; data still is");
    symbols::reset_for_tests();

    // An overlay's function is checked while its overlay is loaded.
    check(symbols::find("hud.quest", &runtime.memory()) != nullptr, "symbols: an overlay's function, overlay absent");
    place_overlay(runtime, 0x0A05E600u, "game_task.ovl", 0x00200000u);
    check(symbols::find("hud.quest", &runtime.memory()) == nullptr,
        "symbols: an overlay's function whose loaded bytes differ is not given out");
    check(!symbols::matches(0x0A17370Cu, "game_task", runtime.memory()) &&
            symbols::matches(0x0A17370Cu, "lobby_task", runtime.memory()) &&
            symbols::matches(0x0A17371Cu, "game_task", runtime.memory()),
        "symbols: a hook at a known function is checked against the bytes there when it is installed");
    place_overlay(runtime, 0x0A05E600u, "lobby_task.ovl", 0x00200000u);
    check(symbols::find("hud.quest", &runtime.memory()) != nullptr,
        "symbols: another overlay in the slot leaves the function unchecked");
    runtime.memory().store32(0x0A05E600u, 0u);
}

// Overlays coming and going, and a quest between them, as the test mod hears
// of them.
void test_events(psprecomp::Runtime &runtime) {
    namespace native = mhp3rd::mods::native;
    constexpr std::uint32_t kSlot = 0x0A05E600u;
    runtime.memory().store32(0x08AB3640u, 0x09000000u); // the character
    runtime.memory().store16(0x09000000u + 0x60472u, 501u);
    place_overlay(runtime, kSlot, "lobby_task.ovl", 0x1000u);
    native::overlay_loaded(runtime, kSlot);
    check(logged("overlay loaded 0A05E600") && !logged("quest started 501"), "events: an overlay loaded, not a quest");
    place_overlay(runtime, kSlot, "game_task.ovl", 0x1000u);
    native::overlay_loaded(runtime, kSlot);
    check(logged("overlay unloaded 0A05E600") && logged("quest started 501"),
        "events: the overlay it replaced went, and the quest overlay starts the quest with its id");
    runtime.memory().store16(0x09000000u + 0x60472u, 1u); // the village writes 1 when it loads
    place_overlay(runtime, kSlot, "lobby_task.ovl", 0x1000u);
    native::overlay_loaded(runtime, kSlot);
    check(logged("quest ended 501"), "events: the quest ends with the id it started with");
    const std::size_t before = native::reports().size();
    runtime.memory().store32(kSlot, 0u); // overwritten by data
    native::frame(runtime);
    bool unloaded = false;
    for (std::size_t i = before; i < native::reports().size(); ++i)
        unloaded = unloaded || native::reports()[i].line == "overlay unloaded 0A05E600";
    check(unloaded, "events: an overlay overwritten by data is unloaded at the next frame");

    // The game flushes twice for one overlay, in one frame.
    const auto loads = [] {
        std::size_t count = 0u;
        for (const auto &report : native::reports()) count += report.line == "overlay loaded 0A05E600" ? 1u : 0u;
        return count;
    };
    place_overlay(runtime, kSlot, "lobby_task.ovl", 0x1000u);
    const std::size_t loads_before = loads();
    native::overlay_loaded(runtime, kSlot);
    native::overlay_loaded(runtime, kSlot);
    check(loads() == loads_before + 1u, "events: two flushes for one overlay in one frame are one load");
    native::frame(runtime);
    native::overlay_loaded(runtime, kSlot);
    check(loads() == loads_before + 2u, "events: the same overlay copied again in a later frame is a new load");
}

// What the test mod draws, through stand-in widgets.
int g_checkbox_calls = 0;
int g_shapes = 0;
void w_text(const char *) {}
void w_text_colored(std::uint32_t, const char *) {}
void w_none() {}
int w_label(const char *) {
    return 0;
}
int w_checkbox(const char *, int *value) {
    ++g_checkbox_calls;
    *value = 1;
    return 1;
}
int w_slider_int(const char *, std::int32_t *, std::int32_t, std::int32_t) {
    return 0;
}
int w_slider_float(const char *, float *, float, float) {
    return 0;
}
int w_input(const char *, char *, std::uint32_t) {
    return 0;
}
void w_line(float, float, float, float, std::uint32_t, float) {}
void w_rect(float, float, float, float, std::uint32_t, int) {
    ++g_shapes;
}
void w_circle(float, float, float, std::uint32_t, int) {}
void w_text_at(float, float, std::uint32_t, const char *) {}
const ymod_widgets kWidgets{1u, sizeof(ymod_widgets), &w_text, &w_text_colored, &w_none, &w_none, &w_label, &w_label,
    &w_checkbox, &w_slider_int, &w_slider_float, &w_input, &w_line, &w_rect, &w_circle, &w_text_at};

void test_ui() {
    namespace native = mhp3rd::mods::native;
    const std::vector<native::Panel> panels = native::panels("test", false);
    const std::vector<native::Panel> overlays = native::panels({}, true);
    check(panels.size() == 1u && panels[0].title == "Test Panel" && native::panels("bad", false).empty(),
        "ui: a mod's panels, for its page");
    check(overlays.size() == 1u && overlays[0].title == "Test Overlay", "ui: every mod's overlays");
    panels[0].draw(&kWidgets, panels[0].user);
    overlays[0].draw(&kWidgets, overlays[0].user);
    check(g_checkbox_calls == 1 && logged("panel switch=1"), "ui: a panel's switch changes the mod's value");
    check(g_shapes == 1 && logged("overlay drawn"), "ui: an overlay draws its shapes");
}

// In C++ a struct's size and its arrays' strides start as this header's, so
// a C++ mod fills in none of them; a C mod sets them (yakumo/mod.h).
void test_sizes() {
    const ymod_quest quest{};
    const ymod_area area{};
    const ymod_area_spot spot{};
    const ymod_symbol symbol{};
    const ymod_table table{};
    const ymod_monster monster{};
    const ymod_weapon weapon{};
    const ymod_armor armor{};
    check(quest.size == sizeof(ymod_quest) && quest.monster_stride == sizeof(ymod_quest_monster) &&
            quest.reward_stride == sizeof(ymod_quest_reward) && quest.supply_stride == sizeof(ymod_quest_supply) &&
            area.size == sizeof(ymod_area) && area.exit_stride == sizeof(ymod_area_exit) &&
            area.arrival_stride == sizeof(ymod_area_arrival) && area.spot_stride == sizeof(ymod_area_spot) &&
            spot.item_stride == sizeof(ymod_area_item) && symbol.size == sizeof(ymod_symbol) &&
            table.size == sizeof(ymod_table) && monster.size == sizeof(ymod_monster) &&
            weapon.size == sizeof(ymod_weapon) && armor.size == sizeof(ymod_armor) &&
            ymod_area_map{}.size == sizeof(ymod_area_map) && area.minimap == nullptr,
        "sdk: in C++ every size and stride starts as this header's");
    // Even left uninitialized, a reserved field is 0 in C++.
    ymod_quest_monster monster_row;
    ymod_quest_reward reward_row;
    ymod_quest_supply supply_row;
    check(monster_row.reserved == 0u && reward_row.reserved == 0u && supply_row.reserved == 0u &&
            ymod_quest_reward{"Wyvern Gem", 1u, 100u, 0u}.reserved == 0u,
        "sdk: in C++ a reserved field starts as 0, and a positional initializer leaves it so");
    // A positional initializer written before the strides keeps its meaning.
    const ymod_area_spot pile{YMOD_SPOT_GATHERING, {1.0f, 2.0f, 3.0f}, 150.0f, 200.0f, 0, nullptr, 0u, 10u, 2u};
    check(pile.uses == 10u && pile.extra_uses == 2u && pile.item_stride == sizeof(ymod_area_item),
        "sdk: the strides come after the fields a positional initializer names");
}

} // namespace

// Hooks that change the hooks from inside their callbacks, and the guest
// calls only a replacement may make.
const ymod_hooks *g_hooks;
const ymod_calls *g_calls;
ymod_mod *g_self;
ymod_hook g_once;
ymod_hook g_added;
int g_seen;
int g_added_seen;
int g_call_result;

int unhook_once(ymod_cpu *cpu, void *) {
    ++g_seen;
    cpu->gpr[4] += 1u;
    g_hooks->remove(g_self, g_once);
    return YMOD_CONTINUE;
}

int count_added(ymod_cpu *, void *) {
    ++g_added_seen;
    return YMOD_CONTINUE;
}

int add_another(ymod_cpu *, void *) {
    ++g_seen;
    if (g_seen == 1) g_added = g_hooks->function(g_self, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE, &count_added, nullptr);
    return YMOD_CONTINUE;
}

int call_from_before(ymod_cpu *cpu, void *) {
    const std::uint32_t args[4]{};
    g_call_result = g_calls->call(cpu, 0x08A00100u, args, nullptr, nullptr);
    return YMOD_CONTINUE;
}

int g_first_call;
int g_second_call;
int call_twice(ymod_cpu *cpu, void *) {
    const std::uint32_t args[4]{};
    g_first_call = g_calls->call(cpu, 0x08A00800u, args, nullptr, nullptr); // v0 = 0x63
    g_second_call = g_calls->call(cpu, 0x08A00100u, args, nullptr, nullptr);
    return YMOD_RETURN;
}

void test_hook_changes(psprecomp::Runtime &runtime) {
    namespace native = mhp3rd::mods::native;
    g_self = native::add_mod("hook_changes", "Hook Changes", ".");
    g_hooks = static_cast<const ymod_hooks *>(native::host()->service(g_self, YMOD_HOOKS, 1u));
    g_calls = static_cast<const ymod_calls *>(native::host()->service(g_self, YMOD_CALLS, 1u));
    store(runtime, 0x08A00300u, {0x00801021u, 0x03E00008u, 0u}); // addu v0, a0, zero; jr ra
    store(runtime, 0x08A00400u, {0x24090007u, 0x25290001u, 0x03E00008u, 0u});

    g_seen = 0;
    g_once = g_hooks->function(g_self, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE, &unhook_once, nullptr);
    const std::uint32_t first = call(runtime, 0x08A00300u, 10u);
    const std::uint32_t second = call(runtime, 0x08A00300u, 10u);
    check(g_seen == 1 && first == 11u && second == 10u && runtime.cpu().pc == kSentinel,
        "native: a BEFORE hook that removes itself, the last at its address, runs the game's code once more");

    g_seen = 0;
    g_once = g_hooks->instruction(g_self, nullptr, 0x08A00404u, &unhook_once, nullptr);
    (void)call(runtime, 0x08A00400u, 0u);
    (void)call(runtime, 0x08A00400u, 0u);
    check(g_seen == 1 && runtime.cpu().gpr[9] == 8u && runtime.cpu().pc == kSentinel,
        "native: an instruction hook that removes itself returns into the code that called it");

    g_seen = 0;
    g_added_seen = 0;
    const ymod_hook adder = g_hooks->function(g_self, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE, &add_another, nullptr);
    (void)call(runtime, 0x08A00300u, 1u);
    const int during = g_added_seen;
    (void)call(runtime, 0x08A00300u, 1u);
    check(during == 0 && g_added_seen == 1 && runtime.cpu().gpr[2] == 1u,
        "native: a hook added from a callback runs from the next call on");
    g_hooks->remove(g_self, adder);
    g_hooks->remove(g_self, g_added);

    // Everything compiled is dropped (a full instruction cache flush whose
    // bytes changed, or a mod's code_changed): hooks compile the game's code
    // again and keep working.
    const ymod_hook again = g_hooks->function(g_self, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE, &count_added, nullptr);
    psprecomp::jit_invalidate(runtime, 0u, 0xFFFFFFFFu);
    native::frame(runtime);
    const int before = g_added_seen;
    check(call(runtime, 0x08A00300u, 4u) == 4u && g_added_seen == before + 1,
        "native: a hook keeps working after all compiled code is dropped");
    runtime.memory().store32(0x08A00300u, 0x24820001u); // addiu v0, a0, 1
    psprecomp::jit_memory_changed(runtime, 0x08A00300u, 0x08A00304u);
    check(call(runtime, 0x08A00300u, 4u) == 5u, "native: a hook runs the game's code as it is after it changed");
    g_hooks->remove(g_self, again);

    g_call_result = 0;
    const ymod_hook caller =
        g_hooks->function(g_self, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE, &call_from_before, nullptr);
    (void)call(runtime, 0x08A00300u, 4u);
    check(g_call_result == -1 && runtime.cpu().gpr[2] == 5u,
        "native: a guest call from a BEFORE hook is refused and the function runs as usual");
    g_hooks->remove(g_self, caller);

    store(runtime, 0x08A00800u, {0x24020063u, 0x03E00008u, 0u}); // addiu v0, zero, 0x63; jr ra
    const ymod_hook twice = g_hooks->function(g_self, nullptr, 0x08A00300u, YMOD_HOOK_REPLACE, &call_twice, nullptr);
    const std::uint32_t called = call(runtime, 0x08A00300u, 4u);
    check(g_first_call == 0 && g_second_call == -1 && called == 0x63u && runtime.cpu().pc == kSentinel,
        "native: a second guest call from one REPLACE callback is refused; the first runs and returns its result");
    g_hooks->remove(g_self, twice);

    // A mod that fails leaves nothing behind that would call it.
    ymod_mod *failing = native::add_mod("failing", "Failing", ".");
    const auto *events = static_cast<const ymod_events *>(native::host()->service(failing, YMOD_EVENTS, 1u));
    const auto *services = static_cast<const ymod_services *>(native::host()->service(failing, YMOD_SERVICES, 1u));
    const auto *ui = static_cast<const ymod_ui *>(native::host()->service(failing, YMOD_UI, 1u));
    static const int kTable = 1;
    g_added_seen = 0;
    (void)g_hooks->function(failing, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE, &count_added, nullptr);
    (void)events->subscribe(failing, YMOD_EVENT_FRAME, [](int, std::uint32_t, void *) { ++g_added_seen; }, nullptr);
    (void)services->publish(failing, "test.failing", 1u, &kTable);
    (void)ui->panel(failing, "Failing Panel", [](const ymod_widgets *, void *) {}, nullptr);
    (void)ui->overlay(failing, "Failing Overlay", [](const ymod_widgets *, void *) {}, nullptr);
    const auto overlays_titled = [](const std::string &title) {
        std::size_t count = 0u;
        for (const native::Panel &panel : native::panels({}, true)) count += panel.title == title ? 1u : 0u;
        return count;
    };
    check(native::panels("failing", false).size() == 1u && overlays_titled("Failing Overlay") == 1u,
        "native: a mod's panel is on its page and its overlay over the game");
    const std::size_t before_withdraw = native::running();
    native::withdraw(failing);
    native::frame(runtime);
    (void)call(runtime, 0x08A00300u, 4u);
    check(g_added_seen == 0 && services->import(g_self, "test.failing", 1u) == nullptr &&
            native::panels("failing", false).empty() && overlays_titled("Failing Overlay") == 0u &&
            native::running() == before_withdraw - 1u,
        "native: a withdrawn mod's hooks, events, services, panels and overlays are gone");
    check(g_hooks->function(failing, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE, &count_added, nullptr) == 0u &&
            events->subscribe(
                failing, YMOD_EVENT_FRAME, [](int, std::uint32_t, void *) {}, nullptr) != 0,
        "native: a withdrawn mod cannot register again");

    // An allocation that cannot fit is refused, not rounded down to nothing.
    const auto *memory = static_cast<const ymod_memory *>(native::host()->service(g_self, YMOD_MEMORY, 1u));
    check(memory != nullptr && memory->allocate(g_self, 0xFFFFFFF0u, 16u) == 0u &&
            memory->allocate(g_self, 0xFFFFFF00u, 16u) == 0u,
        "native: memory.allocate refuses a size past the memory");
    // Memory and subscriptions go back only from the mod that took them: 0,
    // or -1 with the reason on the mod's page.
    ymod_mod *borrower = native::add_mod("borrower", "Borrower", ".");
    const std::uint32_t lent = memory->allocate(g_self, 64u, 16u);
    const int released_by_other = memory->release(borrower, lent);
    const int released = memory->release(g_self, lent);
    char lent_at[16];
    std::snprintf(lent_at, sizeof(lent_at), "0x%08X", lent);
    const std::string not_released = std::string("the memory at ") + lent_at +
        " is not released: the mod did not allocate it, or released it already";
    check(lent != 0u && released_by_other == -1 && reported("Borrower", not_released) && released == 0 &&
            memory->release(g_self, lent) == -1 && reported("Hook Changes", not_released),
        "native: memory.release gives back the mod's own block (0), not another mod's or one given back already "
        "(-1, with the reason)");
    const auto *own_events = static_cast<const ymod_events *>(native::host()->service(g_self, YMOD_EVENTS, 1u));
    const ymod_event_fn listener = [](int, std::uint32_t, void *) {};
    (void)own_events->subscribe(g_self, YMOD_EVENT_FRAME, listener, &g_seen);
    const std::string not_unsubscribed =
        "event 0 is not unsubscribed: the mod has no subscription to it with that function and user";
    check(own_events->unsubscribe(borrower, YMOD_EVENT_FRAME, listener, &g_seen) == -1 &&
            reported("Borrower", not_unsubscribed) &&
            own_events->unsubscribe(g_self, YMOD_EVENT_FRAME, listener, &g_seen) == 0 &&
            own_events->unsubscribe(g_self, YMOD_EVENT_FRAME, listener, &g_seen) == -1 &&
            reported("Hook Changes", not_unsubscribed),
        "native: events.unsubscribe stops the mod's own subscription (0), not another mod's or one stopped already "
        "(-1, with the reason)");

    // A mod stopped by a callback before its own runs in the same call or
    // event is not called: the lists run from copies.
    static ymod_mod *s_quitter = nullptr;
    s_quitter = native::add_mod("quitter", "Quitter", ".");
    ymod_mod *stopper = native::add_mod("stopper", "Stopper", ".");
    const auto *stopper_events = static_cast<const ymod_events *>(native::host()->service(stopper, YMOD_EVENTS, 1u));
    g_added_seen = 0;
    const auto stop_quitter = [](int, std::uint32_t, void *) { native::withdraw(s_quitter); };
    (void)stopper_events->subscribe(stopper, YMOD_EVENT_FILE_LOADED, stop_quitter, nullptr);
    (void)stopper_events->subscribe(
        s_quitter, YMOD_EVENT_FILE_LOADED, [](int, std::uint32_t, void *) { ++g_added_seen; }, nullptr);
    native::file_loaded(0x123u);
    check(g_added_seen == 0, "native: an event is not delivered to a mod stopped earlier in the same event");
    s_quitter = native::add_mod("quitter_hooks", "Quitter Hooks", ".");
    const ymod_hook stop_hook = g_hooks->function(
        stopper, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE,
        [](ymod_cpu *, void *) -> int {
            native::withdraw(s_quitter);
            return YMOD_CONTINUE;
        },
        nullptr);
    (void)g_hooks->function(s_quitter, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE, &count_added, nullptr);
    (void)g_hooks->function(s_quitter, nullptr, 0x08A00300u, YMOD_HOOK_REPLACE, &count_added, nullptr);
    (void)call(runtime, 0x08A00300u, 4u);
    check(g_added_seen == 0 && runtime.cpu().gpr[2] == 5u && runtime.cpu().pc == kSentinel,
        "native: nor a hook of one stopped by an earlier hook at the same call");
    g_hooks->remove(stopper, stop_hook);

    // Panels and overlays come off by the mod that added them: 0, or -1 with
    // the reason when it has none drawn so.
    ymod_mod *drawer = native::add_mod("ui_owner", "UI Owner", ".");
    ymod_mod *bystander = native::add_mod("ui_other", "UI Other", ".");
    const auto *drawing = static_cast<const ymod_ui *>(native::host()->service(drawer, YMOD_UI, 1u));
    static int s_user = 0;
    const ymod_draw_fn draw = [](const ymod_widgets *, void *) {};
    const ymod_draw_fn never_drawn = [](const ymod_widgets *ui, void *) { ui->separator(); };
    (void)drawing->panel(drawer, "Owned Panel", draw, &s_user);
    (void)drawing->overlay(drawer, "Owned Overlay", draw, &s_user);
    const int by_bystander = drawing->remove(bystander, draw, &s_user);
    const bool kept_both = native::panels("ui_owner", false).size() == 1u && overlays_titled("Owned Overlay") == 1u;
    check(by_bystander == -1 && kept_both &&
            reported("UI Other",
                "no panel or overlay is removed: another mod added those drawn by that function with that user"),
        "ui: a mod cannot remove another mod's panels and overlays: -1, and the reason on its page");
    check(drawing->remove(drawer, draw, &s_user) == 0 && native::panels("ui_owner", false).empty() &&
            overlays_titled("Owned Overlay") == 0u,
        "ui: its own mod removes its panel and overlay drawn by that function with that user: 0");
    const std::string none_drawn = "no panel or overlay is removed: none is drawn by that function with that user";
    check(drawing->remove(drawer, draw, &s_user) == -1 && drawing->remove(drawer, never_drawn, nullptr) == -1 &&
            reported("UI Owner", none_drawn),
        "ui: removing what was removed already, or never drawn, is -1, with the reason");

    // A mod that logs every frame does not grow the log without end.
    for (int line = 0; line < 1500; ++line) native::host()->log(g_self, YMOD_LOG_INFO, "chatter");
    check(native::reports().size() <= 1000u && logged("needs a newer Yakumo (mod interface 2, this Yakumo has 1)"),
        "native: the log keeps its latest lines and every problem");
}

// A host feature under a hook takes its address back when the last hook goes,
// and when a hook over it cannot be installed.
void test_host_returns(psprecomp::Runtime &runtime) {
    namespace native = mhp3rd::mods::native;
    ymod_mod *mod = native::add_mod("host_returns", "Host Returns", ".");
    const auto *hooks = static_cast<const ymod_hooks *>(native::host()->service(mod, YMOD_HOOKS, 1u));
    store(runtime, 0x08A00500u, {0x00801021u, 0x03E00008u, 0u}); // addu v0, a0, zero; jr ra
    runtime.register_function(0x08A00500u, &host_feature, "mhp3rd_test_returns");
    g_added_seen = 0;
    const ymod_hook hook = hooks->function(mod, nullptr, 0x08A00500u, YMOD_HOOK_BEFORE, &count_added, nullptr);
    check(call(runtime, 0x08A00500u, 3u) == 6u && g_added_seen == 1, "native: a hook wraps a host feature");
    hooks->remove(mod, hook);
    native::frame(runtime);
    check(call(runtime, 0x08A00500u, 3u) == 6u && g_added_seen == 1 && runtime.cpu().pc == kSentinel,
        "native: the host feature takes its address back when the last hook goes");

    // j, with a j in its delay slot: the game's code there cannot be compiled.
    store(runtime, 0x08A00600u, {0x08280184u, 0x08280188u, 0u});
    runtime.register_function(0x08A00600u, &host_feature, "mhp3rd_test_uncompilable");
    const ymod_hook refused = hooks->function(mod, nullptr, 0x08A00600u, YMOD_HOOK_BEFORE, &count_added, nullptr);
    native::frame(runtime);
    check(refused == 0u &&
            logged("the hook at 0x08A00600 is refused: the game's code there cannot be compiled to run "
                   "under it"),
        "native: a hook that cannot be installed is refused, with 0, and the reason is logged");
    check(call(runtime, 0x08A00600u, 4u) == 8u && g_added_seen == 1 && runtime.cpu().pc == kSentinel,
        "native: a hook that cannot be installed leaves the host feature in place");
    const ymod_hook later = hooks->function(mod, "game_task", 0x0A05E700u, YMOD_HOOK_BEFORE, &count_added, nullptr);
    check(later != 0u, "native: a hook in an overlay not loaded now is taken, to be installed when it loads");
    hooks->remove(mod, later);

    // Only the mod that added a hook removes it.
    ymod_mod *other = native::add_mod("other_remover", "Other Remover", ".");
    const ymod_hook kept = hooks->function(mod, nullptr, 0x08A00500u, YMOD_HOOK_BEFORE, &count_added, nullptr);
    const int by_other = hooks->remove(other, kept);
    native::frame(runtime);
    (void)call(runtime, 0x08A00500u, 3u);
    check(by_other == -1 && g_added_seen == 2 &&
            reported("Other Remover", "hook " + std::to_string(kept) + " is not removed: another mod added it"),
        "native: a mod cannot remove another mod's hook: -1, and the reason on its page");
    const int by_owner = hooks->remove(mod, kept);
    native::frame(runtime);
    (void)call(runtime, 0x08A00500u, 3u);
    check(by_owner == 0 && g_added_seen == 2, "native: its own mod removes it: 0");
    const std::string unknown = "is not removed: no hook has that number: it was never added, or was removed already";
    check(hooks->remove(mod, kept) == -1 && reported("Host Returns", "hook " + std::to_string(kept) + " " + unknown) &&
            hooks->remove(mod, 0u) == -1 && reported("Host Returns", "hook 0 " + unknown) &&
            hooks->remove(mod, 0xFFFFFFF0u) == -1 && reported("Host Returns", "hook 4294967280 " + unknown),
        "native: removing a hook removed already, hook 0 or a number no hook has is -1, with the reason");

    // With the JIT off, nothing would call an instruction hook.
    psprecomp::set_jit_enabled(false);
    const ymod_hook interpreted = hooks->instruction(mod, nullptr, 0x08A00504u, &count_added, nullptr);
    psprecomp::set_jit_enabled(true);
    check(interpreted == 0u &&
            logged("the instruction hook at 0x08A00504 is refused: the JIT is off "
                   "(PSPRECOMP_NO_JIT), and only compiled code runs instruction hooks"),
        "native: an instruction hook is refused while the JIT is off");
}

// Hooks on one address in two overlays that load there: each applies while
// its overlay is loaded.
int g_lobby_seen;
int g_task_seen;
void test_shared_overlay_address(psprecomp::Runtime &runtime) {
    namespace native = mhp3rd::mods::native;
    constexpr std::uint32_t kSlot = 0x0A05E600u;
    constexpr std::uint32_t kFunction = kSlot + 0x200u;
    constexpr std::uint32_t kStep = kSlot + 0x300u;
    ymod_mod *mod = native::add_mod("shared_overlay", "Shared Overlay", ".");
    const auto *hooks = static_cast<const ymod_hooks *>(native::host()->service(mod, YMOD_HOOKS, 1u));
    const auto load = [&runtime](const char *name) {
        place_overlay(runtime, kSlot, name, 0x1000u);
        store(runtime, kFunction, {0x00801021u, 0x03E00008u, 0u});          // addu v0, a0, zero; jr ra
        store(runtime, kStep, {0x24090007u, 0x25290001u, 0x03E00008u, 0u}); // t1 = 7; t1 += 1; jr ra
        native::overlay_loaded(runtime, kSlot);
    };
    load("lobby_task.ovl");
    g_lobby_seen = 0;
    g_task_seen = 0;
    const ymod_hook lobby = hooks->function(
        mod, "lobby_task", kFunction, YMOD_HOOK_BEFORE,
        [](ymod_cpu *, void *) -> int {
            ++g_lobby_seen;
            return YMOD_CONTINUE;
        },
        nullptr);
    const ymod_hook task = hooks->function(
        mod, "game_task", kFunction, YMOD_HOOK_BEFORE,
        [](ymod_cpu *, void *) -> int {
            ++g_task_seen;
            return YMOD_CONTINUE;
        },
        nullptr);
    const ymod_hook lobby_step = hooks->instruction(
        mod, "lobby_task", kStep + 4u,
        [](ymod_cpu *, void *) -> int {
            g_lobby_seen += 10;
            return YMOD_CONTINUE;
        },
        nullptr);
    const ymod_hook task_step = hooks->instruction(
        mod, "game_task", kStep + 4u,
        [](ymod_cpu *, void *) -> int {
            g_task_seen += 10;
            return YMOD_CONTINUE;
        },
        nullptr);
    (void)call(runtime, kFunction, 1u);
    (void)call(runtime, kStep, 0u);
    check(g_lobby_seen == 11 && g_task_seen == 0 && runtime.cpu().gpr[9] == 8u,
        "native: with lobby_task loaded, its hooks at a shared address run and game_task's do not");
    load("game_task.ovl");
    (void)call(runtime, kFunction, 1u);
    (void)call(runtime, kStep, 0u);
    check(g_lobby_seen == 11 && g_task_seen == 11 && runtime.cpu().gpr[9] == 8u,
        "native: once game_task loads there, its hooks run instead");
    load("lobby_task.ovl");
    (void)call(runtime, kFunction, 1u);
    check(g_lobby_seen == 12 && g_task_seen == 11, "native: and lobby_task's again when it comes back");
    for (const ymod_hook hook : {lobby, task, lobby_step, task_step}) hooks->remove(mod, hook);
}

// Quests mods add: encrypted with the keys in memory, served as files, and
// the game's id-to-file code given the file for a mod's quest id.
void test_quests(psprecomp::Runtime &runtime) {
    namespace quests = mhp3rd::mods::quests;
    mhp3rd::game::GuestRam ram(runtime.memory());
    const std::uint32_t multipliers[4] = {5897u, 15859u, 29819u, 45953u};
    const std::uint32_t moduli[4] = {65437u, 65449u, 65479u, 65521u};
    for (std::uint32_t i = 0; i < 4u; ++i) {
        ram.store32(mhp3rd::game::kQuestMultipliers + i * 4u, multipliers[i]);
        ram.store32(mhp3rd::game::kQuestModuli + i * 4u, moduli[i]);
    }
    for (std::uint32_t i = 0; i < 16u; ++i) ram.store8(mhp3rd::game::kQuestSalt + i, static_cast<std::uint8_t>(i));

    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "yakumo_native_quests";
    std::error_code cleared;
    std::filesystem::remove_all(folder, cleared);
    std::filesystem::create_directories(folder);
    std::vector<std::uint8_t> payload(0x100u, 0u);
    payload[0] = 0x10u;
    payload[0x10u + 0x1Cu] = 0x31u; // 62001
    payload[0x10u + 0x1Du] = 0xF2u;
    std::ofstream(folder / "q.bin", std::ios::binary)
        .write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
    const std::map<mhp3rd::mods::FileId, mhp3rd::mods::Resolution::Source> wanted{
        {0x6001u, {"quest_mod", folder / "q.bin", {}}}};

    std::map<mhp3rd::mods::FileId, mhp3rd::mods::Resolution::Source> files{{0x2000u, {"other", "x", {}}}};
    check(!quests::install(runtime, wanted, files, folder / "cache") && files.size() == 1u,
        "quests: not served on code other than the traced");

    // The game's id-to-file code where it finds no file, standing in.
    store(runtime, 0x088C1778u, {0x30A9FFFFu});
    store(runtime, 0x088C18B4u, {0x3410FFFFu, 0x3402FFFFu, 0x120200ACu, 0u, 0x966221B2u, 0x03E00008u, 0u});
    store(runtime, 0x088C1B70u, {0x03E00008u, 0u});
    const std::size_t mods_before = mhp3rd::mods::native::running();
    check(quests::install(runtime, wanted, files, folder / "cache") && files.size() == 2u && files.contains(0x6001u) &&
            quests::find(62001u) != nullptr && quests::served().size() == 1u,
        "quests: a mod's quest is served as a new file");
    std::vector<std::uint8_t> file;
    {
        // Closed before the folder is removed: Windows keeps an open file.
        std::ifstream in(files.at(0x6001u).path, std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::optional<mhp3rd::game::QuestKeys> keys = mhp3rd::game::read_quest_keys(ram);
    check(keys && mhp3rd::game::decrypt_quest(file, *keys) == payload,
        "quests: the served file decrypts with the game's keys to the mod's quest");

    const auto file_for = [&runtime](std::uint32_t quest) {
        runtime.cpu().gpr[9] = quest;        // t1
        runtime.cpu().gpr[19] = 0x08A00000u; // s3, read by the stand-in
        (void)call(runtime, 0x088C18B4u, 0u);
        return runtime.cpu().gpr[16]; // s0
    };
    check(file_for(62001u) == 0x6001u && runtime.cpu().pc == kSentinel, "quests: the mod's quest id loads its file");
    check(file_for(62002u) == 0xFFFFu && runtime.cpu().pc == kSentinel, "quests: another id still finds no file");
    check(mhp3rd::mods::native::running() == mods_before, "quests: Yakumo's own hook is not counted as a mod");
    check(mhp3rd::mods::tables::placement("quest.events")->address == 0x0A188B1Cu,
        "quests: not among the event quests when the event loader is not as traced");
    quests::reset_for_tests();

    // The event quest loader where it has worked out the file into s3, then
    // returning; and the game's event quest list in lobby_task.
    store(runtime, 0x0888F9F4u, {0x30A5FFFFu, 0x3402EE48u, 0u, 0x24A3253Au});
    store(runtime, 0x0888FA44u, {0x24A22187u, 0x3053FFFFu, 0x3C1409FCu, 0x03E00008u, 0u});
    std::map<mhp3rd::mods::FileId, mhp3rd::mods::Resolution::Source> again{{0x2000u, {"other", "x", {}}}};
    check(quests::install(runtime, wanted, again, folder / "cache") && again.contains(0x6001u),
        "quests: served again, with the event quest loader there too");
    const auto event_file = [&runtime](std::uint32_t quest) {
        runtime.cpu().gpr[5] = quest;    // a1
        runtime.cpu().gpr[19] = 0x0F9Bu; // s3: the game's file
        (void)call(runtime, 0x0888FA4Cu, 0u);
        return runtime.cpu().gpr[19];
    };
    check(event_file(62001u) == 0x6001u && event_file(60001u) == 0x0F9Bu,
        "quests: the event quest loader reads a mod's quest from its file, the game's from theirs");
    const std::optional<mhp3rd::mods::tables::Placement> events = mhp3rd::mods::tables::placement("quest.events");
    check(events && events->address != 0x0A188B1Cu && events->rows == 43u,
        "quests: the event quest list is made longer by one");
    place_overlay(runtime, 0x0A05E600u, "lobby_task.ovl", 0x00200000u);
    store(runtime, 0x0A0B1264u, {0x3C030A19u, 0u, 0u, 0u, 0u, 0x94628B1Cu});
    store(runtime, 0x0A0B1294u, {0x3C020A19u, 0x24428B1Eu});
    store(runtime, 0x0A0B1324u, {0x3C020A19u, 0x94458B1Cu});
    store(runtime, 0x0A0B1338u, {0x3C020A19u, 0x24528B1Eu});
    runtime.memory().store32(0x0A188B1Cu, 60001u | (60002u << 16u));
    runtime.memory().store32(0x0A188B20u, 0u);
    for (int load = 0; load < 2; ++load) {
        mhp3rd::mods::tables::overlay_loaded(runtime);
        quests::overlay_loaded(runtime);
    }
    const std::uint32_t list = events ? events->address : 0u;
    check(runtime.memory().load16(list) == 60001u && runtime.memory().load16(list + 2u) == 60002u &&
            runtime.memory().load16(list + 4u) == 62001u && runtime.memory().load16(list + 6u) == 0u,
        "quests: once lobby_task loads, the mod's quest follows the game's in the list, once");
    const std::uint32_t next = list + 2u;
    check(runtime.memory().load32(0x0A0B1298u) == (0x24420000u | (next & 0xFFFFu)) &&
            runtime.memory().load32(0x0A0B1294u) == (0x3C020000u | ((next + 0x8000u) >> 16u)) &&
            runtime.memory().load32(0x0A0B1278u) == (0x94620000u | (list & 0xFFFFu)),
        "quests: the code that reads the list, and the next id in it, reads the longer list");

    // A quest a mod describes, built and served from memory.
    loader_stand_ins(runtime);
    mhp3rd::game::QuestSpec spec;
    spec.id = 62002u;
    spec.map = 8u;
    spec.stars = 6u;
    spec.target = 58u;
    spec.monsters = {{58u, 1u, 1086.0f, 1.0f, 255.0f, 0x1Eu, 0x13u, 100u}};
    spec.texts = {"Built", "Hunt the Tigrex", "Time Over", "A quest made by a mod", "Tigrex", "Guild"};
    const std::optional<std::vector<std::uint8_t>> built = mhp3rd::game::build_quest(spec);
    std::string why;
    check(built && quests::add(runtime, *built, "builder", &why) && file_for(62002u) == 0x6002u &&
            quests::find(62002u) != nullptr && quests::find(62002u)->name == "Built" &&
            mhp3rd::mods::added::serves(0x6002u) && event_file(62002u) == 0x6002u,
        "quests: a quest a mod built is served from memory, for the developer tools and the counter");
    check(mhp3rd::mods::tables::placement("quest.events")->rows == 44u,
        "quests: the event quest list grows by one for it");
    std::vector<std::uint8_t> game_quest = *built;
    game_quest[0x94u + 0x1Cu] = 101u;
    game_quest[0x94u + 0x1Du] = 0u;
    check(!quests::add(runtime, *built, "builder", &why) && why.find("served already") != std::string::npos &&
            !quests::add(runtime, game_quest, "builder", &why) && why.find("mod's id") != std::string::npos &&
            !quests::add(runtime, {1u, 2u}, "builder", &why),
        "quests: an id served already, a game quest's id and a broken payload are refused");

    // The list reaches 101 rows: the quests past that are served but not at
    // the counter, and those that got a row stay there.
    bool all_served = true;
    for (std::uint16_t id = 64003u; id <= 64062u; ++id) { // ids the later tests do not use
        spec.id = id;
        const std::optional<std::vector<std::uint8_t>> another = mhp3rd::game::build_quest(spec);
        all_served = all_served && another && quests::add(runtime, *another, "builder", &why);
    }
    const std::optional<mhp3rd::mods::tables::Placement> full = mhp3rd::mods::tables::placement("quest.events");
    const std::uint32_t at = full ? full->address : 0u;
    for (std::uint32_t row = 2u; full && row < full->rows; ++row) runtime.memory().store16(at + row * 2u, 0u);
    quests::overlay_loaded(runtime);
    std::uint32_t listed = 0u;
    while (full && listed + 2u < full->rows && runtime.memory().load16(at + (listed + 2u) * 2u) != 0u) ++listed;
    check(all_served && full && full->rows == 101u && listed == 59u && runtime.memory().load16(at + 4u) == 62001u &&
            runtime.memory().load16(at + 60u * 2u) == 64059u,
        "quests: once the list cannot grow, the quests that got a row are still at the counter, the rest are not");
    mhp3rd::mods::added::reset_for_tests();
    mhp3rd::mods::tables::reset_for_tests();
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
}

// Game tables made longer, on stand-ins for the code that reads them: the
// executable's first model file of each set, and the quest overlay's kinds.
void test_tables(psprecomp::Runtime &runtime) {
    namespace tables = mhp3rd::mods::tables;
    tables::reset_for_tests();
    check(tables::find("monster.models") != nullptr && tables::find("monster.models")->row == 2u &&
            tables::find("no.such") == nullptr && tables::all().size() >= 2u,
        "tables: known by name");
    check(tables::placement("monster.models")->address == 0x08982DD0u &&
            tables::placement("monster.models")->rows == 74u && !tables::placement("no.such"),
        "tables: one not extended is where the game keeps it");
    std::string why;
    check(
        !tables::extend(runtime, "no.such", 80u, &why) && why == "no such table", "tables: an unknown one is refused");
    check(!tables::extend(runtime, "monster.models", 75u, &why) && why.find("not as traced") != std::string::npos,
        "tables: not moved when the code that reads it is not the game's");
    check(tables::extend(runtime, "monster.models", 20u) == 0x08982DD0u,
        "tables: no more rows than the game has leaves it where it is");

    // model(set): the game's 0x088564E4, set from 1; the search's address
    // and count at 0x08856684.
    store(runtime, 0x088564E4u, {0x00042040u, 0x3C020898u, 0x24422DD0u, 0x00441021u, 0x9442FFFEu, 0x03E00008u, 0u});
    store(runtime, 0x08856684u, {0x3C020898u, 0u, 0x24452DD0u, 0u, 0x2407004Au});
    for (std::uint32_t set = 1u; set <= 74u; ++set)
        runtime.memory().store16(0x08982DD0u + (set - 1u) * 2u, 5000u + set);
    check(call(runtime, 0x088564E4u, 58u) == 5058u, "tables: the stand-in reads the game's table");
    check(!tables::extend(runtime, "monster.models", 256u, &why) && why.find("reaches 255") != std::string::npos,
        "tables: no more rows than the game's index can reach");
    const std::optional<std::uint32_t> copy = tables::extend(runtime, "monster.models", 75u);
    check(copy && *copy != 0x08982DD0u && tables::placement("monster.models")->address == *copy &&
            tables::placement("monster.models")->rows == 75u,
        "tables: extended, the table moves to memory of its own");
    check(copy && runtime.memory().load16(*copy + 57u * 2u) == 5058u && runtime.memory().load16(*copy + 74u * 2u) == 0u,
        "tables: the copy has the game's rows, and the new row starts at zero");
    if (copy) runtime.memory().store16(*copy + 74u * 2u, 9075u);
    check(call(runtime, 0x088564E4u, 75u) == 9075u && call(runtime, 0x088564E4u, 58u) == 5058u,
        "tables: the game's code reads the copy, the new row too");
    check(runtime.memory().load32(0x08856694u) == 0x2407004Bu && runtime.memory().load32(0x0885668Cu) != 0x24452DD0u,
        "tables: every reference and the count follow the table");
    check(tables::extend(runtime, "monster.models", 75u) == copy, "tables: asked again, it stays where it is");
    const std::optional<std::uint32_t> longer = tables::extend(runtime, "monster.models", 90u);
    check(longer && longer != copy && call(runtime, 0x088564E4u, 75u) == 9075u &&
            runtime.memory().load32(0x08856694u) == 0x2407005Au,
        "tables: made longer again, the rows mods wrote go with it");
    runtime.memory().store32(0x0885668Cu, 0x24450000u);
    check(!tables::extend(runtime, "monster.models", 100u, &why) && why.find("not as traced") != std::string::npos,
        "tables: not moved again over another mod's change to its code");

    // A word of data that holds a table's address: the list of the armor
    // parts' tables, which the count of a piece's slots reads. Left at the
    // game's table, a piece past the game's read a count past the three the
    // equipment screens have room for.
    constexpr std::uint32_t kWaists = 0x089832E0u;
    tables::write_traced_code_for_tests(runtime, "armor.waist");
    const std::optional<std::uint32_t> waists = tables::extend(runtime, "armor.waist", 215u);
    check(waists && *waists != 0x0898A6E4u && runtime.memory().load32(kWaists) == *waists,
        "tables: a word of data that holds the table's address follows it");
    const std::optional<std::uint32_t> more_waists = tables::extend(runtime, "armor.waist", 230u);
    check(more_waists && more_waists != waists && runtime.memory().load32(kWaists) == *more_waists,
        "tables: and each move after");
    runtime.memory().store32(kWaists, 0x08900000u);
    check(!tables::extend(runtime, "armor.waist", 240u, &why) && why.find("not as traced") != std::string::npos,
        "tables: not moved again over another mod's change to a word that holds its address");

    // The kinds, in the quest overlay: extended before the overlay loads.
    constexpr std::uint32_t kSlot = 0x0A05E600u;
    place_overlay(runtime, kSlot, "lobby_task.ovl", 0x00200000u);
    const std::optional<std::uint32_t> kinds = tables::extend(runtime, "monster.kinds", 76u);
    check(kinds && *kinds != 0x0A17FE84u && runtime.memory().load32(*kinds + 58u * 8u) == 0u,
        "tables: an overlay's table moves before its overlay is there, with no rows yet");
    const auto load_game_task = [&runtime, kSlot] {
        place_overlay(runtime, kSlot, "game_task.ovl", 0x00200000u);
        store(runtime, 0x0A08CC64u, {0x3C020A18u, 0u, 0x2442FE84u});
        store(runtime, 0x0A08CE38u, {0x3C030A18u, 0x2463FE84u});
        store(runtime, 0x0A08CF10u, {0x28A2004Bu});
        for (std::uint32_t kind = 0u; kind < 75u; ++kind) runtime.memory().store32(0x0A17FE84u + kind * 8u, kind);
        tables::overlay_loaded(runtime);
    };
    load_game_task();
    check(kinds && runtime.memory().load32(*kinds + 58u * 8u) == 58u &&
            runtime.memory().load32(0x0A08CF10u) == 0x28A2004Cu &&
            runtime.memory().load32(0x0A08CC64u) == (0x3C020000u | ((*kinds + 0x8000u) >> 16u)) &&
            runtime.memory().load32(0x0A08CC6Cu) == (0x24420000u | (*kinds & 0xFFFFu)),
        "tables: when the overlay loads, its rows are copied and its code points at the copy");
    if (kinds) runtime.memory().store32(*kinds + 75u * 8u, 0x75u);
    load_game_task();
    check(kinds && runtime.memory().load32(*kinds + 75u * 8u) == 0x75u &&
            runtime.memory().load32(0x0A08CF10u) == 0x28A2004Cu,
        "tables: loaded again, the overlay's code points at the copy, which keeps the mods' rows");
    place_overlay(runtime, kSlot, "game_task.ovl", 0x00200000u);
    store(runtime, 0x0A08CC64u, {0x3C020A18u, 0u, 0x2442FE84u});
    store(runtime, 0x0A08CE38u, {0x3C030A18u, 0x12345678u});
    std::ostringstream said;
    std::streambuf *previous = std::cerr.rdbuf(said.rdbuf());
    tables::overlay_loaded(runtime);
    tables::overlay_loaded(runtime);
    std::cerr.rdbuf(previous);
    check(runtime.memory().load32(0x0A08CC64u) == 0x3C020A18u &&
            said.str() ==
                "[mods] table monster.kinds is not moved: the overlay's code that reads it is not as traced\n",
        "tables: an overlay whose code is not as traced is left alone, and said once");

    // The same through the mods' service.
    mhp3rd::mods::native::attach(runtime);
    ymod_mod *host = mhp3rd::mods::native::add_mod("yakumo.tables_test", "Tables test", "");
    const auto *service =
        static_cast<const ymod_tables *>(mhp3rd::mods::native::host()->service(host, YMOD_TABLES, 1u));
    ymod_table entry{sizeof(ymod_table)};
    check(service != nullptr && service->find("monster.kinds", &entry) == 0 && entry.address == *kinds &&
            entry.rows == 76u && entry.game_rows == 75u && entry.row == 8u && std::string(entry.overlay) == "game_task",
        "tables: the service gives a table where it is now");
    ymod_table first{sizeof(ymod_table)};
    check(service != nullptr && service->get(0u, &first) == 0 && service->get(1000u, &first) != 0 &&
            service->find("no.such", &entry) != 0,
        "tables: the service lists every table");
    check(service != nullptr && service->extend(host, "monster.kinds", 76u) == *kinds &&
            service->extend(host, "no.such", 2u) == 0u && logged("table no.such is not extended: no such table"),
        "tables: the service extends, and says why it cannot");
    tables::reset_for_tests();
}

// A pack as both games keep one: a count, then an offset and a size per
// entry (an empty entry is absent), then the entries, 16-byte aligned.
std::vector<std::uint8_t> pack_of(const std::vector<std::vector<std::uint8_t>> &entries) {
    const auto aligned = [](std::size_t n) { return (n + 15u) & ~std::size_t{15u}; };
    std::vector<std::uint8_t> out(aligned(4u + 8u * entries.size()), 0u);
    const auto put = [&out](std::size_t at, std::uint32_t v) { std::memcpy(out.data() + at, &v, 4u); };
    put(0u, static_cast<std::uint32_t>(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].empty()) continue;
        put(4u + 8u * i, static_cast<std::uint32_t>(out.size()));
        put(8u + 8u * i, static_cast<std::uint32_t>(entries[i].size()));
        out.insert(out.end(), entries[i].begin(), entries[i].end());
        out.resize(aligned(out.size()), 0u);
    }
    return out;
}

std::uint32_t word_at(const std::vector<std::uint8_t> &data, std::size_t at) {
    std::uint32_t v = 0u;
    std::memcpy(&v, data.data() + at, 4u);
    return v;
}

std::vector<std::uint8_t> entry_of(const std::vector<std::uint8_t> &pack, std::uint32_t index) {
    const std::uint32_t offset = word_at(pack, 4u + 8u * index), size = word_at(pack, 8u + 8u * index);
    return size ? std::vector<std::uint8_t>(pack.begin() + offset, pack.begin() + offset + size)
                : std::vector<std::uint8_t>{};
}

// A PMO "102" with `meshes` meshes: tables after the mesh table, GE data in
// the next file (its GE base is its size).
std::vector<std::uint8_t> small_pmo(std::uint16_t meshes) {
    const std::uint32_t groups = 0x40u + 0x30u * meshes;
    std::vector<std::uint8_t> pmo(groups + 0x30u, 0u);
    std::memcpy(pmo.data(),
        "pmo\0"
        "102\0",
        8u);
    const auto put = [&pmo](std::size_t at, std::uint32_t v) { std::memcpy(pmo.data() + at, &v, 4u); };
    put(0x08u, static_cast<std::uint32_t>(pmo.size()) + 100u);
    std::memcpy(pmo.data() + 0x1Cu, &meshes, 2u);
    put(0x20u, 0x40u);
    put(0x24u, groups);
    put(0x2Cu, groups + 0x10u);
    put(0x30u, groups + 0x20u);
    put(0x34u, static_cast<std::uint32_t>(pmo.size()));
    for (std::uint16_t m = 0; m < meshes; ++m) {
        const float scale = 500.0f;
        for (int axis = 0; axis < 3; ++axis) std::memcpy(pmo.data() + 0x40u + 0x30u * m + 4u * axis, &scale, 4u);
        put(0x40u + 0x30u * m + 0x20u, 0x80000007u);
        put(0x40u + 0x30u * m + 0x28u, 0x00010001u);
    }
    return pmo;
}

// Weapons mods add: a row of the class's table, its model through the
// model-to-file code, its name and description in the text block, and one in
// the box once the village loads; on stand-ins for the game's code and data.
void test_equipment(psprecomp::Runtime &runtime) {
    namespace equipment = mhp3rd::mods::equipment;
    namespace tables = mhp3rd::mods::tables;
    tables::reset_for_tests();
    equipment::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
    loader_stand_ins(runtime);
    tables::write_traced_code_for_tests(runtime, "weapon.great_sword");
    auto &memory = runtime.memory();
    // Great sword 3: model 0x1D, rarity 2, price 1000, description 3.
    constexpr std::uint32_t kSwords = 0x08997AA0u;
    for (std::uint32_t i = 0; i < 92u * 28u; i += 4u) memory.store32(kSwords + i, 0u);
    memory.store16(kSwords + 3u * 28u, 0x1Du);
    memory.store8(kSwords + 3u * 28u + 2u, 2u);
    memory.store32(kSwords + 3u * 28u + 4u, 1000u);
    memory.store32(kSwords + 3u * 28u + 8u, 0x005A010Au);
    memory.store16(kSwords + 3u * 28u + 0x18u, 3u);
    memory.store16(0x089CD1F8u, 40u); // the class's models
    // The text block: table 5 the names (92), table 6 the descriptions (60).
    constexpr std::uint32_t kBlock = 0x08A40640u;
    const auto make_table = [&memory](std::uint32_t at, std::uint32_t rows, const char *prefix) {
        std::uint32_t text = at + 4u * (rows + 1u);
        for (std::uint32_t i = 0; i < rows; ++i) {
            memory.store32(at + 4u * i, text - at);
            const std::string s = prefix + std::to_string(i);
            for (const char c : s) memory.store8(text++, static_cast<std::uint8_t>(c));
            memory.store8(text++, 0u);
        }
        memory.store32(at + 4u * rows, 0xFFFFFFFFu);
    };
    make_table(kBlock + 0x1000u, 92u, "Sword ");
    make_table(kBlock + 0x2000u, 60u, "About ");
    memory.store32(kBlock + 5u * 4u, 0x1000u);
    memory.store32(kBlock + 6u * 4u, 0x2000u);
    // The model-to-file code: the model in a1, the class in s0; a model past
    // the count gives 0xDEAD, any other the class's first file plus it.
    store(runtime, 0x088A59DCu,
        {0x34030028u, 0u, 0x00A3102Bu, 0x1040000Eu, 0u, 0x340805D1u, 0u, 0u, 0u, 0u, 0x00A82021u, 0x3082FFFFu,
            0x03E00008u, 0u});
    store(runtime, 0x088A5A24u, {0x3402DEADu, 0x03E00008u, 0u});

    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "yakumo_weapon_test";
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "blade.pac", std::ios::binary) << "BLADE";
    ymod_mod *mod = mhp3rd::mods::native::add_mod("test.weapons", "Weapon test", folder.string());
    const auto *service =
        static_cast<const ymod_equipment *>(mhp3rd::mods::native::host()->service(mod, YMOD_EQUIPMENT, 1u));
    ymod_weapon blade{sizeof(ymod_weapon), "black_blade", "great_sword", 3, "blade.pac", "Black Blade",
        "A blade a mod added.", 1234u, 1};
    check(service != nullptr && service->add_weapon(mod, &blade) == 92, "equipment: a weapon is added as the next id");
    const std::optional<tables::Placement> swords = tables::placement("weapon.great_sword");
    const std::uint32_t row = swords ? swords->address + 92u * 28u : 0u;
    check(swords && swords->address != kSwords && swords->rows >= 93u && memory.load16(row) == 40u &&
            memory.load8(row + 2u) == 2u && memory.load32(row + 4u) == 1234u && memory.load32(row + 8u) == 0x005A010Au,
        "equipment: its record is the base's, with a model past the class's and its own price");
    const auto file_for = [&runtime](std::uint32_t model) {
        runtime.cpu().gpr[5] = model;
        runtime.cpu().gpr[16] = 0u;
        return call(runtime, 0x088A59DCu, 0u);
    };
    const std::uint32_t served = file_for(40u);
    check(served >= mhp3rd::mods::added::kFirstFreeId && mhp3rd::mods::added::serves(served) &&
            file_for(10u) == 1499u && file_for(41u) == 0xDEADu,
        "equipment: its model gives its own file; the game's models and others past the count as before");
    equipment::frame(runtime);
    mhp3rd::game::GuestRam ram(memory);
    check(mhp3rd::game::text_count(ram, kBlock, 5) == 93u &&
            mhp3rd::game::text_entry(ram, kBlock, 5, 92u) == "Black Blade" &&
            mhp3rd::game::text_entry(ram, kBlock, 5, 91u) == "Sword 91" &&
            mhp3rd::game::text_entry(ram, kBlock, 6, 60u) == "A blade a mod added." &&
            memory.load16(row + 0x18u) == 60u,
        "equipment: its name at its id and its description, whose index the record keeps, are in the text block");
    // The game loads the block again: the strings are put back.
    memory.store32(kBlock + 5u * 4u, 0x1000u);
    equipment::frame(runtime);
    check(
        mhp3rd::game::text_entry(ram, kBlock, 5, 92u) == "Black Blade", "equipment: and again when the block reloads");
    // In the village with a character: one in the box, once.
    for (std::uint32_t i = 0; i < 1000u * 12u; i += 4u) memory.store32(mhp3rd::game::kEquipmentBox + i, 0u);
    memory.store8(mhp3rd::game::kEquipmentBox, 1u); // slot 0 taken
    memory.store8(mhp3rd::game::kEquipmentBox + 1u, 5u);
    memory.store16(mhp3rd::game::kEquipmentBox + 2u, 3u);
    equipment::frame(runtime);
    check(memory.load8(mhp3rd::game::kEquipmentBox + 12u) == 0u, "equipment: not given outside the village");
    place_overlay(runtime, 0x0A05E600u, "lobby_task.ovl", 0x00200000u);
    memory.store16(mhp3rd::game::kHunterName, 'A');
    equipment::frame(runtime);
    equipment::frame(runtime);
    check(memory.load8(mhp3rd::game::kEquipmentBox + 12u) == 1u &&
            memory.load8(mhp3rd::game::kEquipmentBox + 13u) == 5u &&
            memory.load16(mhp3rd::game::kEquipmentBox + 14u) == 92u &&
            memory.load8(mhp3rd::game::kEquipmentBox + 24u) == 0u,
        "equipment: in the village, one is put in the first free slot of the box, once");
    ymod_weapon taken = blade;
    ymod_weapon unknown = blade;
    unknown.id = "unknown";
    unknown.kind = "spear";
    ymod_weapon no_base = blade;
    no_base.id = "no_base";
    no_base.based_on = 92;
    ymod_weapon no_model = blade;
    no_model.id = "no_model";
    no_model.model = "missing.pac";
    ymod_weapon old{sizeof(std::uint32_t)};
    check(service != nullptr && service->add_weapon(mod, &taken) == -1 &&
            logged("weapon black_blade is not added: the id \"black_blade\" is taken or empty") &&
            service->add_weapon(mod, &unknown) == -1 && service->add_weapon(mod, &no_base) == -1 &&
            service->add_weapon(mod, &no_model) == -1 && service->add_weapon(mod, &old) == -1 &&
            service->add_weapon(mod, nullptr) == -1,
        "equipment: a taken id, an unknown class, a base past the class's, a missing model and short structs are refused");

    // Armor: a row of the part's table, its male and female models through
    // the armor model-to-file code, its name and description, one in the box.
    tables::write_traced_code_for_tests(runtime, "armor.head");
    constexpr std::uint32_t kHeads = 0x08987EE4u;
    for (std::uint32_t i = 0; i < 256u * 40u; i += 4u) memory.store32(kHeads + i, 0u);
    memory.store16(kHeads + 3u * 40u, 0x22u);
    memory.store16(kHeads + 3u * 40u + 2u, 0x23u);
    memory.store8(kHeads + 3u * 40u + 4u, 0x1Fu);
    memory.store32(kHeads + 3u * 40u + 8u, 500u);
    memory.store32(kHeads + 3u * 40u + 0x14u, 0x00120010u);
    memory.store16(kHeads + 3u * 40u + 0x1Au, 3u);
    make_table(kBlock + 0x4000u, 256u, "Helm ");
    make_table(kBlock + 0x7000u, 147u, "Worn ");
    memory.store32(kBlock + 29u * 4u, 0x4000u);
    memory.store32(kBlock + 30u * 4u, 0x7000u);
    // The armor model-to-file code: the model (a1) in v0 and the base (a2) in
    // v1 at the hooked instruction, then their sum.
    store(runtime, 0x088A5958u,
        {0x27BDFFF0u, 0xAFBF0008u, 0x00A01021u, 0x00C01821u, 0x8FBF0008u, 0u, 0x27BD0010u, 0x00431021u, 0x3042FFFFu,
            0x03E00008u, 0u});
    std::ofstream(folder / "helm_m.pmo", std::ios::binary) << "HELM-M";
    std::ofstream(folder / "helm_f.pmo", std::ios::binary) << "HELM-F";
    ymod_armor helm{sizeof(ymod_armor), "black_helm", "head", 3, "helm_m.pmo", "helm_f.pmo", "Black Helm",
        "A helm a mod added.", 2000u, 1};
    ymod_armor plain = helm;
    plain.id = "plain_helm";
    plain.model_female = nullptr;
    plain.give = 0;
    plain.price = 0;
    check(service != nullptr && service->add_armor(mod, &helm) == 256 && service->add_armor(mod, &plain) == 257,
        "equipment: armor pieces are added as the part's next ids");
    const std::optional<tables::Placement> heads = tables::placement("armor.head");
    const std::uint32_t helm_row = heads ? heads->address + 256u * 40u : 0u;
    const std::uint32_t plain_row = heads ? heads->address + 257u * 40u : 0u;
    check(heads && heads->address != kHeads && heads->rows >= 258u && memory.load16(helm_row) == 1000u &&
            memory.load16(helm_row + 2u) == 1001u && memory.load8(helm_row + 4u) == 0x1Fu &&
            memory.load32(helm_row + 8u) == 2000u && memory.load32(helm_row + 0x14u) == 0x00120010u &&
            memory.load16(plain_row) == 1002u && memory.load16(plain_row + 2u) == 1002u &&
            memory.load32(plain_row + 8u) == 500u,
        "equipment: an armor record is the base's, with its own models (one number for both when only a male "
        "model is given) and price");
    const auto armor_file = [&runtime](std::uint32_t model, std::uint32_t base, std::uint32_t sex, std::uint32_t kind) {
        runtime.cpu().gpr[5] = model;
        runtime.cpu().gpr[6] = base;
        runtime.cpu().gpr[16] = 0x089E892Cu + 2u * (sex * 7u + kind);
        return call(runtime, 0x088A5958u, 0u);
    };
    const std::uint32_t male_file = armor_file(1000u, 0x500u, 0u, 4u);
    const std::uint32_t female_file = armor_file(1001u, 0x279u, 1u, 4u);
    check(mhp3rd::mods::added::serves(male_file) && mhp3rd::mods::added::serves(female_file) &&
            male_file != female_file && armor_file(1002u, 0x279u, 1u, 4u) == armor_file(1002u, 0x500u, 0u, 4u) &&
            armor_file(0x22u, 0x500u, 0u, 4u) == 0x522u && armor_file(1000u, 0x300u, 0u, 0u) == 0x6E8u &&
            armor_file(1000u, 0x500u, 2u, 4u) == 0x8E8u,
        "equipment: an armor model gives its own file for each sex; the game's models, the same number for another "
        "part and places past the table of bases as before");
    equipment::frame(runtime);
    check(mhp3rd::game::text_entry(ram, kBlock, 29, 256u) == "Black Helm" &&
            mhp3rd::game::text_entry(ram, kBlock, 30, 147u) == "A helm a mod added." &&
            memory.load16(helm_row + 0x1Au) == 147u,
        "equipment: an armor piece's name is at its id and its description's index in its record");
    equipment::frame(runtime);
    bool helm_given = false;
    for (std::uint32_t slot = 0; slot < 8u; ++slot) {
        const std::uint32_t at = mhp3rd::game::kEquipmentBox + slot * 12u;
        helm_given |= memory.load8(at + 1u) == 4u && memory.load16(at + 2u) == 256u;
    }
    check(helm_given, "equipment: the armor asked for is put in the box as a head piece");
    ymod_armor no_part = helm;
    no_part.id = "no_part";
    no_part.part = "tail";
    ymod_armor past = helm;
    past.id = "past";
    past.based_on = 300;
    ymod_armor no_helm_model = helm;
    no_helm_model.id = "no_helm_model";
    no_helm_model.model_male = "missing.pmo";
    ymod_armor no_female = helm;
    no_female.id = "no_female";
    no_female.model_female = "missing.pmo";
    ymod_armor short_armor{sizeof(std::uint32_t)};
    check(service != nullptr && service->add_armor(mod, &helm) == -1 &&
            logged("armor black_helm is not added: the id \"black_helm\" is taken or empty") &&
            service->add_armor(mod, &no_part) == -1 &&
            logged("armor no_part is not added: \"tail\" is not an armor part") &&
            service->add_armor(mod, &past) == -1 && service->add_armor(mod, &no_helm_model) == -1 &&
            service->add_armor(mod, &no_female) == -1 && service->add_armor(mod, &short_armor) == -1 &&
            service->add_armor(mod, nullptr) == -1 && equipment::armor_parts().size() == 5u,
        "equipment: a taken id, an unknown part, a base past the part's, missing models and short structs are "
        "refused");
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    equipment::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
    tables::reset_for_tests();
}

void test_maps(psprecomp::Runtime &runtime) {
    namespace maps = mhp3rd::mods::maps;
    namespace game = mhp3rd::game;
    using Bytes = std::vector<std::uint8_t>;
    const auto bytes = [](std::string_view s) { return Bytes(s.begin(), s.end()); };
    const auto entry = [](const Bytes &pack, std::size_t i) {
        const auto entries = game::read_pack(pack);
        return entries && i < entries->size() ? Bytes((*entries)[i].begin(), (*entries)[i].end()) : Bytes{};
    };

    // Packs.
    const Bytes pack = game::write_pack({bytes("zero"), {}, bytes("two!")});
    check(pack.size() == 64u && entry(pack, 0) == bytes("zero") && entry(pack, 1).empty() &&
            entry(pack, 2) == bytes("two!"),
        "packs: entries are written at 16-byte steps, an empty one as absent, and read back");
    const auto replaced = game::replace_entries(pack, {{1u, bytes("one")}});
    check(replaced && entry(*replaced, 0) == bytes("zero") && entry(*replaced, 1) == bytes("one") &&
            entry(*replaced, 2) == bytes("two!"),
        "packs: an entry is replaced and the others kept");
    check(!game::read_pack(bytes("\xFF\xFF")) && !game::replace_entries(pack, {{3u, bytes("x")}}) &&
            !game::read_pack(Bytes{2, 0, 0, 0, 0x40, 0, 0, 0, 8, 0, 0, 0}),
        "packs: something that is not a pack, or an entry past its last, is refused");

    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
    loader_stand_ins(runtime);
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "yakumo_maps_test";
    std::filesystem::create_directories(folder);
    const auto write = [&folder](const char *name, std::string_view text) {
        std::ofstream(folder / name, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
    };
    write("model.pmo",
        std::string_view("pmo\0"
                         "102\0MINE",
            12));
    write("ge.bin", "GEOMETRY");
    write("tex.tmh", ".TMH0.14 mine");
    write("ground.bin", "4.00 ground");
    write("walls.bin", "4.00 walls");
    write("bad.pmo", "not a model");
    ymod_mod *mod = mhp3rd::mods::native::add_mod("test.maps", "Map test", folder.string());
    const auto *service = static_cast<const ymod_maps *>(mhp3rd::mods::native::host()->service(mod, YMOD_MAPS, 1u));
    ymod_area castle{sizeof(ymod_area), "castle", 8, 1, "model.pmo", "ge.bin", "tex.tmh", "ground.bin", "walls.bin"};
    check(service != nullptr && service->add_area(mod, &castle) == 0 && maps::find("castle") != nullptr &&
            maps::find("castle")->map == 8u && maps::find("castle")->area == 1u,
        "maps: an area is added in place of a map's area");
    ymod_area taken = castle;
    ymod_area bad_map = castle;
    bad_map.id = "bad_map";
    bad_map.map = 20;
    ymod_area bad_model = castle;
    bad_model.id = "bad_model";
    bad_model.model = "bad.pmo";
    ymod_area missing = castle;
    missing.id = "missing";
    missing.walls = "none.bin";
    ymod_area old{sizeof(std::uint32_t)};
    check(service != nullptr && service->add_area(mod, &taken) == -1 &&
            logged("area castle is not added: the id \"castle\" is taken or empty") &&
            service->add_area(mod, &bad_map) == -1 &&
            logged("area bad_map is not added: map 20 is not one of the game's") &&
            service->add_area(mod, &bad_model) == -1 &&
            logged("area bad_model is not added: its model is not a PMO \"102\"") &&
            service->add_area(mod, &missing) == -1 && service->add_area(mod, &old) == -1 &&
            service->add_area(mod, nullptr) == -1 && maps::find("bad_map") == nullptr,
        "maps: a taken id, a map that is not the game's, a file not in its format, a missing file and short structs "
        "are refused");
    std::string why;
    const Bytes pmo = bytes(std::string_view("pmo\0"
                                             "102\0",
        8)); // the bytes a PMO "102" starts with
    check(!maps::add_area(runtime, {"tex", 8, 1, pmo, bytes("g"), bytes("TMH?"), bytes("4.00"), bytes("4.00")}, &why) &&
            why == "its textures are not a TMH" &&
            !maps::add_area(
                runtime, {"hits", 8, 1, pmo, bytes("g"), bytes(".TMH"), bytes("4.00"), bytes("HITS")}, &why) &&
            why == "its ground or walls are not a collision block (\"4.00\")" &&
            !maps::add_area(runtime, {"ge", 8, 1, pmo, {}, bytes(".TMH"), bytes("4.00"), bytes("4.00")}, &why) &&
            why == "it has no GE data",
        "maps: textures, collision and GE data are checked too");

    // The game's files for the arena's floor, and the map's collision.
    const Bytes area_pack =
        game::write_pack({bytes("THEIR MODEL"), bytes("SCENE"), bytes("SETTINGS"), bytes("THEIR TMH")});
    const Bytes hits_pack = game::write_pack({bytes("MAP TMH"), bytes("CAMP 1"), bytes("CAMP 2"), bytes("FLOOR 1"),
        bytes("FLOOR 2"), bytes("CAMP POINTS"), bytes("FLOOR POINTS")});
    maps::read_files_with_for_tests([&](mhp3rd::mods::FileId file) -> std::optional<Bytes> {
        if (file == 0x138Bu) return area_pack;
        if (file == 0x1262u) return hits_pack;
        if (file == 0x1263u) return bytes("not a pack");
        if (file == 0x1390u) return Bytes(16u, 0u);    // map 9's first area's GE data
        if (file == 0x138Au) return bytes("THEIR GE"); // the arena's first area's GE data
        if (file == 0x138Cu) return Bytes(0x10000u, 0u);
        return std::nullopt;
    });
    auto &memory = runtime.memory();
    memory.store32(0x08AB3640u, 0x09000000u); // the character
    memory.store16(0x09000000u + 0x60472u, 62600u);
    check(maps::serve(runtime, 0x138Cu) == nullptr, "maps: nothing is served before a quest is played in the area");
    check(maps::play_quest_in("castle", 62600u) && !maps::play_quest_in("nowhere", 62601u),
        "maps: a quest is played in an area that was added, and only such");
    check(maps::serve(runtime, 0x138Cu) == nullptr, "maps: nor while game_sub, which holds the tables, is not loaded");
    place_overlay(runtime, 0x0A001780u, "game_sub.ovl", 0x00050000u);
    memory.store16(0x0A0457C4u + 16u, 0x1389u); // map 8's first area pack
    memory.store16(0x0A04579Cu + 16u, 0x138Au); // and GE data
    memory.store8(0x0A0457ECu + 8u, 2u);        // and its two areas
    memory.store8(0x0A0457ECu + 9u, 2u);        // and map 9's
    memory.store16(0x0A0457C4u + 18u, 0x1391u);
    memory.store16(0x0A04579Cu + 18u, 0x1390u);
    const auto served = [&](mhp3rd::mods::FileId file) {
        const auto b = maps::serve(runtime, file);
        return b ? *b : Bytes{};
    };
    const Bytes their_pack = served(0x138Bu);
    check(served(0x138Cu) == bytes("GEOMETRY"), "maps: the area's GE data is the mod's");
    check(entry(their_pack, 0) ==
                bytes(std::string_view("pmo\0"
                                       "102\0MINE",
                    12)) &&
            entry(their_pack, 1) == bytes("SCENE") && entry(their_pack, 2) == bytes("SETTINGS") &&
            entry(their_pack, 3) == bytes(".TMH0.14 mine"),
        "maps: the area's pack has the mod's model and textures, the game's scene and settings");
    const Bytes collision = served(0x1262u);
    check(entry(collision, 1) == bytes("CAMP 1") && entry(collision, 2) == bytes("CAMP 2") &&
            entry(collision, 3) == bytes("4.00 ground") && entry(collision, 4) == bytes("4.00 walls") &&
            entry(collision, 5) == bytes("CAMP POINTS") && entry(collision, 6) == bytes("FLOOR POINTS"),
        "maps: the map's collision has the mod's ground and walls in the area's blocks, the others the game's");
    check(served(0x1389u).empty() && served(0x138Eu).empty() && served(0x1263u).empty(),
        "maps: the map's other areas and other files are the game's");
    // Through the loader the game asks.
    runtime.cpu().gpr[5] = 0x138Cu;
    check(call(runtime, 0x08864E8Cu, 0x08B3F040u) == 8u, "maps: the loader answers the mod's GE data's size");
    runtime.cpu().gpr[5] = 0x138Cu;
    runtime.cpu().gpr[6] = 0x08B00000u;
    check(call(runtime, 0x08863CDCu, 0x08B3F040u) == 1u && memory.load32(0x08B00000u) == 0x4D4F4547u,
        "maps: and copies it where the game asks");
    memory.store16(0x09000000u + 0x60472u, 501u);
    runtime.cpu().gpr[5] = 0x138Cu;
    check(call(runtime, 0x08864E8Cu, 0x08B3F040u) == 0x777u && maps::serve(runtime, 0x138Bu) == nullptr,
        "maps: on another quest the game's own files load");
    // GE data larger than the game's is not served, nor the rest of the area.
    check(
        maps::add_area(runtime, {"big", 8, 0, pmo, Bytes(64, 1u), bytes(".TMH"), bytes("4.00"), bytes("4.00")}, &why) &&
            maps::play_quest_in("big", 62604u),
        "maps: an area with large GE data is added");
    memory.store16(0x09000000u + 0x60472u, 62604u);
    check(maps::serve(runtime, 0x138Au) == nullptr && maps::serve(runtime, 0x1389u) == nullptr &&
            maps::serve(runtime, 0x1262u) == nullptr,
        "maps: GE data larger than the game's area's is refused, and the area's other files with it");
    // An area of a map whose collision pack is not as expected.
    check(maps::add_area(
              runtime, {"odd", 9, 0, pmo, bytes("g"), bytes(".TMH"), bytes("4.00"), bytes("4.00"), "Map test"}, &why) &&
            maps::play_quest_in("odd", 62602u),
        "maps: a second area is added");
    memory.store16(0x09000000u + 0x60472u, 62602u);
    check(maps::serve(runtime, 0x1263u) == nullptr && maps::serve(runtime, 0x1391u) == nullptr,
        "maps: with a collision pack that is not as expected, the game's area and collision are loaded");

    // A quest played in an area names it; one that names an area not added is refused.
    const auto *quests = static_cast<const ymod_quests *>(mhp3rd::mods::native::host()->service(mod, YMOD_QUESTS, 1u));
    const ymod_quest_monster monster{"tigrex", 1, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100};
    ymod_quest quest{sizeof(ymod_quest), 62603u, 5, 1, 30u, 0u, 0u, "tigrex", 1u, &monster, 1u, {}, "nowhere"};
    check(quests != nullptr && quests->add(mod, &quest) == -1 &&
            logged("quest 62603 is not added: its area \"nowhere\" has not been added"),
        "maps: a quest naming an area that was not added is refused");
    // One that names an added area is on its map, whatever `map` says, and a
    // monster of area 0 starts in the added area; another area is kept.
    const ymod_quest_monster pair[] = {
        {"tigrex", 0, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100}, {"tigrex", 3, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100}};
    ymod_quest in_castle = quest;
    in_castle.id = 62605u;
    in_castle.map = 300;
    in_castle.area = "castle";
    in_castle.monsters = pair;
    in_castle.monster_count = 2u;
    const auto built_quest = [&](std::uint32_t id) -> std::optional<std::vector<std::uint8_t>> {
        const mhp3rd::mods::FileId file = *mhp3rd::mods::p3rd::quest_file(id);
        runtime.cpu().gpr[5] = file;
        const std::uint32_t size = call(runtime, 0x08864E8Cu, 0x08B3F040u);
        runtime.cpu().gpr[5] = file;
        runtime.cpu().gpr[6] = 0x08C00000u;
        (void)call(runtime, 0x08863CDCu, 0x08B3F040u);
        std::vector<std::uint8_t> encrypted(size);
        memory.copy_out(0x08C00000u, encrypted);
        game::GuestRam keys_ram(memory);
        if (const auto keys = game::read_quest_keys(keys_ram)) return game::decrypt_quest(encrypted, *keys);
        return std::nullopt;
    };
    std::optional<std::vector<std::uint8_t>> built;
    if (quests != nullptr && quests->add(mod, &in_castle) == 62605) built = built_quest(62605u);
    const auto u32_at = [&built](std::size_t at) { return word_at(*built, at); };
    const auto area_of = [&](std::size_t monster) {
        const std::uint32_t spawns = u32_at(u32_at(0x20u) + 12u);
        return static_cast<std::uint32_t>(
            (*built)[spawns + 0x30u * monster + 6u] | ((*built)[spawns + 0x30u * monster + 7u] << 8u));
    };
    const std::optional<game::QuestRecord> record = built ? game::read_quest_record(*built) : std::nullopt;
    check(built && record && record->map == 8u && area_of(0) == 1u && area_of(1) == 3u,
        "maps: a quest in an added area is on its map, its monsters of area 0 in the added area");
    // Its monsters from a mod built with wider ones, as a later SDK's: read by
    // their stride, the extra bytes ignored. A stride of 0, as a C mod that
    // forgot it gives, is refused with the reason.
    struct WideMonster {
        ymod_quest_monster monster;
        const char *later;
        std::int32_t more;
    };
    const WideMonster wide_pair[] = {{pair[0], "not a monster", -1}, {pair[1], "not a monster", -1}};
    ymod_quest wide = in_castle;
    wide.id = 62606u;
    wide.monsters = &wide_pair[0].monster;
    wide.monster_stride = sizeof(WideMonster);
    built = quests != nullptr && quests->add(mod, &wide) == 62606 ? built_quest(62606u) : std::nullopt;
    check(built && area_of(0) == 1u && area_of(1) == 3u,
        "maps: a quest's monsters wider than this header's are read by their stride, their extra bytes ignored");
    ymod_quest unstrided = in_castle;
    unstrided.id = 62607u;
    unstrided.monster_stride = 0u;
    check(quests != nullptr && quests->add(mod, &unstrided) == -1 &&
            logged("quest 62607 is not added: monster_stride is 0, less than the " +
                std::to_string(sizeof(ymod_quest_monster)) +
                " bytes of a ymod_quest_monster: set it to sizeof(ymod_quest_monster)"),
        "maps: a quest whose monsters' stride is 0 is refused, with the reason");

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
}

// A quest played in several areas of a mod's, and areas that give their own
// points (where the hunter comes in and leaves, the camp's things).
void test_map_points(psprecomp::Runtime &runtime) {
    namespace maps = mhp3rd::mods::maps;
    namespace game = mhp3rd::game;
    using Bytes = std::vector<std::uint8_t>;
    const auto bytes = [](std::string_view s) { return Bytes(s.begin(), s.end()); };
    const auto entry = [](const Bytes &pack, std::size_t i) {
        const auto entries = game::read_pack(pack);
        return entries && i < entries->size() ? Bytes((*entries)[i].begin(), (*entries)[i].end()) : Bytes{};
    };
    const auto f32 = [](const Bytes &b, std::size_t at) {
        const std::uint32_t bits = word_at(b, at);
        float value{};
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    const auto u16 = [](const Bytes &b, std::size_t at) {
        return static_cast<std::uint16_t>(b[at] | (b[at + 1u] << 8u));
    };

    // The table: groups by kind, records of 32 bytes from the next 16 bytes.
    maps::Points points;
    points.exits = {{100.0f, 5.0f, -200.0f, 300.0f, 500.0f, 1u, 2u}};
    points.arrivals = {{1.0f, 2.0f, 3.0f, 0x8000u, maps::kQuestStart, 0u}, {4.0f, 5.0f, 6.0f, 0x4000u, 1u, 2u}};
    points.spots = {{maps::SpotKind::Bed, 7.0f, 8.0f, 9.0f, 130.0f, 300.0f, 0xC001u}};
    points.cart = maps::Cart{10.0f, 11.0f, 12.0f, 0x7000u};
    const Bytes table = maps::write_points(points);
    // Four groups: 4 + 48 bytes, so the records start at 0x40.
    check(table.size() == 0x40u + 5u * 32u && word_at(table, 0u) == 4u && word_at(table, 4u) == 0u &&
            word_at(table, 8u) == 1u && word_at(table, 12u) == 0x40u && word_at(table, 16u) == 1u &&
            word_at(table, 20u) == 2u && word_at(table, 24u) == 0x60u && word_at(table, 28u) == 2u &&
            word_at(table, 32u) == 1u && word_at(table, 36u) == 0xA0u && word_at(table, 40u) == 4u &&
            word_at(table, 44u) == 1u && word_at(table, 48u) == 0xC0u,
        "points: a count, then each group's kind, count and offset, its records after them from 16 bytes on");
    check(f32(table, 0x40u) == 100.0f && f32(table, 0x44u) == 5.0f && f32(table, 0x48u) == -200.0f &&
            f32(table, 0x4Cu) == 1.0f && f32(table, 0x50u) == 300.0f && f32(table, 0x54u) == 500.0f &&
            u16(table, 0x58u) == 0u && u16(table, 0x5Cu) == 2u && table[0x5Eu] == 1u,
        "points: an exit has its middle, radius, height, door and the area it leads to");
    check(f32(table, 0x60u) == 1.0f && u16(table, 0x78u) == 0x8000u && table[0x7Eu] == 0xFFu &&
            f32(table, 0x80u) == 4.0f && f32(table, 0x90u) == 1.0f && u16(table, 0x98u) == 0x4000u &&
            u16(table, 0x9Au) == 1u && u16(table, 0x9Cu) == 2u && table[0x9Eu] == 1u && table[0x9Fu] == 1u,
        "points: an arrival has its place, facing, door and the area it comes from (0xFF the quest's start)");
    check(f32(table, 0xA8u) == 9.0f && f32(table, 0xB0u) == 130.0f && f32(table, 0xB4u) == 300.0f &&
            u16(table, 0xB8u) == 0xC001u && u16(table, 0xBCu) == 0u && u16(table, 0xBEu) == 7u &&
            f32(table, 0xC0u) == 10.0f && u16(table, 0xD8u) == 0x7000u && word_at(table, 0xDCu) == 0u,
        "points: a thing to use has its kind, radius, height and facing; the cart its place and facing");
    const Bytes empty = maps::write_points({});
    check(empty.size() == 16u && word_at(empty, 0u) == 0u, "points: with nothing, a table of no groups");
    maps::Points only_exits;
    only_exits.exits = {{0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0u, 0u}, {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0u, 1u}};
    const Bytes exits_table = maps::write_points(only_exits);
    check(word_at(exits_table, 0u) == 1u && word_at(exits_table, 12u) == 0x10u && exits_table.size() == 0x50u &&
            u16(exits_table, 0x10u + 32u + 26u) == 1u && exits_table[0x10u + 32u + 31u] == 1u,
        "points: one group's records start at 16, each with its index");

    // Areas through the service, with points and without.
    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
    loader_stand_ins(runtime);
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "yakumo_map_points_test";
    std::filesystem::create_directories(folder);
    const auto write = [&folder](const char *name, std::string_view text) {
        std::ofstream(folder / name, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
    };
    write("model.pmo",
        std::string_view("pmo\0"
                         "102\0CAMP",
            12));
    write("floor.pmo",
        std::string_view("pmo\0"
                         "102\0YARD",
            12));
    write("ge.bin", "CAMP GE");
    write("floor_ge.bin", "YARD GE");
    write("tex.tmh", ".TMH camp");
    write("ground.bin", "4.00 camp ground");
    write("walls.bin", "4.00 camp walls");
    ymod_mod *mod = mhp3rd::mods::native::add_mod("test.points", "Points test", folder.string());
    const auto *service = static_cast<const ymod_maps *>(mhp3rd::mods::native::host()->service(mod, YMOD_MAPS, 1u));
    const ymod_area_exit door{{5980.0f, -20.0f, 5000.0f}, 300.0f, 500.0f, 1, 0};
    const ymod_area_arrival ins[] = {
        {{5950.0f, 0.0f, 6100.0f}, 0x8000, YMOD_AREA_QUEST_START, 0}, {{5950.0f, 3.0f, 5600.0f}, 0, 1, 0}};
    const ymod_area_spot things[] = {{YMOD_SPOT_SUPPLY_BOX, {6200.0f, 0.0f, 6300.0f}, 150.0f, 200.0f, 0x7000},
        {YMOD_SPOT_BED, {5480.0f, 0.0f, 6390.0f}, 130.0f, 300.0f, 0xC001}};
    const ymod_area_arrival cart{{6000.0f, 0.0f, 6800.0f}, 0x4000, 0, 0};
    ymod_area camp{sizeof(ymod_area), "camp", 8, 0, "model.pmo", "ge.bin", "tex.tmh", "ground.bin", "walls.bin"};
    camp.exits = &door;
    camp.exit_count = 1u;
    camp.arrivals = ins;
    camp.arrival_count = 2u;
    camp.spots = things;
    camp.spot_count = 2u;
    camp.cart = &cart;
    // An older mod's struct ends at the walls: no points.
    ymod_area yard{
        offsetof(ymod_area, exits), "yard", 8, 1, "floor.pmo", "floor_ge.bin", "tex.tmh", "ground.bin", "walls.bin"};
    yard.exits = &door; // past its size: not read
    yard.exit_count = 1u;
    check(service != nullptr && service->add_area(mod, &camp) == 0 && service->add_area(mod, &yard) == 0 &&
            maps::find("camp") != nullptr && maps::find("yard") != nullptr,
        "points: an area with points is added, and one of a mod built before them");
    const auto refused = [&](ymod_area area, const char *id, const std::string &line) {
        area.id = id;
        return service != nullptr && service->add_area(mod, &area) == -1 && maps::find(id) == nullptr &&
            logged("area " + std::string(id) + " is not added: " + line);
    };
    std::vector<ymod_area_exit> many(17u, door);
    ymod_area too_many = camp;
    too_many.exits = many.data();
    too_many.exit_count = 17u;
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    ymod_area_exit bad_exits[] = {door, door, door};
    bad_exits[0].to = 255;
    bad_exits[1].to = -1;
    bad_exits[2].door = 256;
    ymod_area_exit odd_exits[] = {door, door, door, door, door, door};
    odd_exits[0].radius = 0.0f;
    odd_exits[1].height = -1.0f;
    odd_exits[2].radius = kInfinity;
    odd_exits[3].height = kNaN;
    odd_exits[4].at.x = kNaN;
    odd_exits[5].at.z = -kInfinity;
    ymod_area_arrival bad_ins[] = {ins[0], ins[0], ins[0], ins[0], ins[0]};
    bad_ins[0].facing = 0x10000;
    bad_ins[1].facing = -1;
    bad_ins[2].from = 255;
    bad_ins[3].from = -2;
    bad_ins[4].door = -1;
    ymod_area_spot bad_things[] = {things[0], things[0]};
    bad_things[0].kind = 8;
    bad_things[1].facing = 0x10000;
    ymod_area_spot odd_things[] = {things[0], things[0], things[0], things[0]};
    odd_things[0].radius = 0.0f;
    odd_things[1].height = -5.0f;
    odd_things[2].radius = kInfinity;
    odd_things[3].at.y = kNaN;
    const std::string exit_line = "an exit's area or door is out of range";
    const std::string odd_exit_line = "an exit's place is not a number or its size is not above 0";
    const std::string in_line = "an arrival's facing, area or door is out of range";
    const std::string thing_line = "a thing to use is not a gathering point, supply box, delivery box, bed, "
                                   "ballista or cannon, or its facing is out of range";
    const std::string odd_thing_line = "a thing to use's place is not a number or its size is not above 0";
    bool all_refused = refused(too_many, "many", "it has more than 16 exits, arrivals or things to use");
    for (std::size_t i = 0; i < std::size(bad_exits); ++i) {
        ymod_area a = camp;
        a.exits = &bad_exits[i];
        all_refused = refused(a, ("exit" + std::to_string(i)).c_str(), exit_line) && all_refused;
    }
    for (std::size_t i = 0; i < std::size(odd_exits); ++i) {
        ymod_area a = camp;
        a.exits = &odd_exits[i];
        all_refused = refused(a, ("odd_exit" + std::to_string(i)).c_str(), odd_exit_line) && all_refused;
    }
    for (std::size_t i = 0; i < std::size(odd_things); ++i) {
        ymod_area a = camp;
        a.spots = &odd_things[i];
        a.spot_count = 1u;
        all_refused = refused(a, ("odd_thing" + std::to_string(i)).c_str(), odd_thing_line) && all_refused;
    }
    ymod_area_arrival nowhere_in = ins[1];
    nowhere_in.at.y = kInfinity;
    ymod_area lost = camp;
    lost.arrivals = &nowhere_in;
    lost.arrival_count = 1u;
    all_refused = refused(lost, "lost", "an arrival's place is not a number") && all_refused;
    ymod_area_arrival cart_nowhere = cart;
    cart_nowhere.at.x = kNaN;
    ymod_area lost_cart = camp;
    lost_cart.cart = &cart_nowhere;
    all_refused = refused(lost_cart, "lost_cart", "its cart's place is not a number") && all_refused;
    // An exit back into its own area, and a cart anywhere but a camp.
    ymod_area_exit loop = door;
    loop.to = 0;
    ymod_area looping = camp;
    looping.exits = &loop;
    all_refused = refused(looping, "looping", "an exit leads to its own area") && all_refused;
    ymod_area floor_cart = camp;
    floor_cart.area = 1;
    floor_cart.exits = nullptr;
    floor_cart.exit_count = 0u;
    all_refused = refused(floor_cart, "floor_cart", "only a camp (area 0) has a cart") && all_refused;
    for (std::size_t i = 0; i < std::size(bad_ins); ++i) {
        ymod_area a = camp;
        a.arrivals = &bad_ins[i];
        a.arrival_count = 1u;
        all_refused = refused(a, ("in" + std::to_string(i)).c_str(), in_line) && all_refused;
    }
    for (std::size_t i = 0; i < std::size(bad_things); ++i) {
        ymod_area a = camp;
        a.spots = &bad_things[i];
        a.spot_count = 1u;
        all_refused = refused(a, ("thing" + std::to_string(i)).c_str(), thing_line) && all_refused;
    }
    ymod_area_arrival bad_cart = cart;
    bad_cart.facing = 70000;
    ymod_area cart_off = camp;
    cart_off.cart = &bad_cart;
    all_refused = refused(cart_off, "cart", "its cart's facing is out of range") && all_refused;
    check(all_refused,
        "points: too many, an exit to no area or its own or with a door past 255, an arrival's facing, area or door "
        "out of range, a thing that is not the camp's, a cart's facing out of range or a cart off the camp, a place "
        "that is not a finite number and a size not above 0 (infinite, not a number) are refused");
    // Strides below the first SDK's elements, or so wide the array passes 4 GiB.
    const auto stride_line = [](const char *field, std::size_t stride, std::size_t first, const char *type) {
        return std::string(field) + " is " + std::to_string(stride) + ", less than the " + std::to_string(first) +
            " bytes of a " + type + ": set it to sizeof(" + type + ")";
    };
    constexpr std::size_t kExitEnd = offsetof(ymod_area_exit, door) + sizeof(std::int32_t);
    constexpr std::size_t kArrivalEnd = offsetof(ymod_area_arrival, door) + sizeof(std::int32_t);
    constexpr std::size_t kSpotEnd = offsetof(ymod_area_spot, item_stride) + sizeof(std::uint32_t);
    ymod_area short_exits = camp;
    short_exits.exit_stride = static_cast<std::uint32_t>(kExitEnd - 4u);
    ymod_area unstrided_arrivals = camp;
    unstrided_arrivals.arrival_stride = 0u;
    ymod_area cart_alone = camp;
    cart_alone.arrivals = nullptr;
    cart_alone.arrival_count = 0u;
    cart_alone.arrival_stride = 0u;
    ymod_area short_spots = camp;
    short_spots.spot_stride = static_cast<std::uint32_t>(offsetof(ymod_area_spot, item_stride));
    const ymod_area_exit two_doors[] = {door, door};
    ymod_area endless = camp;
    endless.exits = two_doors;
    endless.exit_count = 2u;
    endless.exit_stride = 0x80000000u;
    check(refused(short_exits, "short_exits", stride_line("exit_stride", kExitEnd - 4u, kExitEnd, "ymod_area_exit")) &&
            refused(unstrided_arrivals, "unstrided_arrivals",
                stride_line("arrival_stride", 0u, kArrivalEnd, "ymod_area_arrival")) &&
            refused(cart_alone, "cart_alone", stride_line("arrival_stride", 0u, kArrivalEnd, "ymod_area_arrival")) &&
            refused(short_spots, "short_spots",
                stride_line("spot_stride", offsetof(ymod_area_spot, item_stride), kSpotEnd, "ymod_area_spot")) &&
            refused(endless, "endless", "exit_stride is 2147483648: 2 elements that far apart pass 4 GiB"),
        "points: an exit, arrival (the cart's too) or spot stride below its first element, 0, or one whose array "
        "passes 4 GiB is refused with the reason, before anything of the array is read");
    std::string why;
    const Bytes pmo = bytes(std::string_view("pmo\0"
                                             "102\0",
        8));
    maps::Points crowded;
    crowded.spots.resize(17u);
    check(!maps::add_area(runtime,
              {"crowded", 8, 0, pmo, bytes("g"), bytes(".TMH"), bytes("4.00"), bytes("4.00"), "Points test", crowded},
              &why) &&
            why == "it has more than 16 exits, arrivals or things to use",
        "points: the service's own limit holds for areas added from inside too");

    // Played together: on map 8, one of a map 9 area or in place of the camp again is refused.
    check(maps::add_area(runtime, {"elsewhere", 9, 0, pmo, bytes("g"), bytes(".TMH"), bytes("4.00"), bytes("4.00")}) &&
            maps::add_area(runtime, {"camp2", 8, 0, pmo, bytes("g"), bytes(".TMH"), bytes("4.00"), bytes("4.00")}),
        "points: two more areas are added");
    check(maps::play_quest_in("camp", 62700u) && maps::play_quest_in("yard", 62700u) &&
            maps::play_quest_in("camp", 62700u) && !maps::play_quest_in("elsewhere", 62700u, &why) &&
            why == "its area elsewhere is on map 9, its area camp on map 8" &&
            !maps::play_quest_in("camp2", 62700u, &why) &&
            why == "its areas camp and camp2 both take the place of area 0" &&
            !maps::play_quest_in("nowhere", 62700u, &why) && why == "there is no area \"nowhere\"",
        "points: a quest is played in two areas of a map; one on another map, in place of the same area, or not "
        "added is refused, and naming one again changes nothing");

    // The game's files: the arena's two areas and its collision, with its points tables.
    const Bytes camp_pack = game::write_pack({bytes("THEIR CAMP"), bytes("CAMP SCENE"), {}, bytes("THEIR CAMP TMH")});
    const Bytes floor_pack =
        game::write_pack({bytes("THEIR FLOOR"), bytes("FLOOR SCENE"), {}, bytes("THEIR FLOOR TMH")});
    const Bytes hits_pack = game::write_pack({bytes("MAP TMH"), bytes("CAMP 1"), bytes("CAMP 2"), bytes("FLOOR 1"),
        bytes("FLOOR 2"), bytes("CAMP POINTS"), bytes("FLOOR POINTS")});
    maps::read_files_with_for_tests([&](mhp3rd::mods::FileId file) -> std::optional<Bytes> {
        if (file == 0x1389u) return camp_pack;
        if (file == 0x138Au) return Bytes(0x1000u, 0u);
        if (file == 0x138Bu) return floor_pack;
        if (file == 0x138Cu) return Bytes(0x1000u, 0u);
        if (file == 0x1262u) return hits_pack;
        if (file == 0x1263u) return game::write_pack({bytes("TMH"), bytes("A"), bytes("B")});
        return std::nullopt;
    });
    auto &memory = runtime.memory();
    place_overlay(runtime, 0x0A001780u, "game_sub.ovl", 0x00050000u);
    memory.store16(0x0A0457C4u + 16u, 0x1389u);
    memory.store16(0x0A04579Cu + 16u, 0x138Au);
    memory.store8(0x0A0457ECu + 8u, 2u); // the arena has two areas
    memory.store32(0x08AB3640u, 0x09000000u);
    memory.store16(0x09000000u + 0x60472u, 62700u);
    const auto served = [&](mhp3rd::mods::FileId file) {
        const auto b = maps::serve(runtime, file);
        return b ? *b : Bytes{};
    };
    check(served(0x138Au) == bytes("CAMP GE") && served(0x138Cu) == bytes("YARD GE") &&
            entry(served(0x1389u), 3) == bytes(".TMH camp") && entry(served(0x1389u), 1) == bytes("CAMP SCENE") &&
            entry(served(0x138Bu), 0) ==
                bytes(std::string_view("pmo\0"
                                       "102\0YARD",
                    12)),
        "points: each of the quest's areas has its pack and GE data served");
    const Bytes collision = served(0x1262u);
    check(entry(collision, 1) == bytes("4.00 camp ground") && entry(collision, 2) == bytes("4.00 camp walls") &&
            entry(collision, 3) == bytes("4.00 camp ground") && entry(collision, 4) == bytes("4.00 camp walls") &&
            entry(collision, 0) == bytes("MAP TMH"),
        "points: the map's collision has every one of the quest's areas' blocks");
    const Bytes camp_points = entry(collision, 5);
    check(camp_points.size() == 0x40u + 6u * 32u && word_at(camp_points, 0u) == 4u &&
            f32(camp_points, 0x40u) == 5980.0f && camp_points[0x5Eu] == 1u && f32(camp_points, 0x60u) == 5950.0f &&
            camp_points[0x7Eu] == 0xFFu && camp_points[0x9Eu] == 1u && u16(camp_points, 0xBEu) == 5u &&
            u16(camp_points, 0xDEu) == 7u && f32(camp_points, 0xE0u) == 6000.0f && u16(camp_points, 0xF8u) == 0x4000u,
        "points: the camp's points table is its exits, arrivals, supply box, bed and cart");
    check(entry(collision, 6) == bytes("FLOOR POINTS"), "points: an area that gives none keeps the game's table");
    // The same camp from a mod built with wider elements, as a later SDK's
    // with fields of its own past these: read by its strides, the extra bytes
    // ignored, it has the same table.
    struct WideExit {
        ymod_area_exit exit;
        std::uint32_t later[3];
    };
    struct WideArrival {
        ymod_area_arrival arrival;
        std::uint32_t later[5];
    };
    struct WideSpot {
        ymod_area_spot spot;
        std::uint32_t later[2];
    };
    const WideExit wide_door[] = {{door, {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}}};
    WideArrival wide_ins[2] = {};
    WideSpot wide_things[2] = {};
    for (std::size_t i = 0; i < 2u; ++i) {
        wide_ins[i] = {ins[i], {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}};
        wide_things[i] = {things[i], {0xFFFFFFFFu, 0xFFFFFFFFu}};
    }
    const WideArrival wide_cart{cart, {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}};
    ymod_area wide = camp;
    wide.id = "wide_camp";
    wide.exits = &wide_door[0].exit;
    wide.exit_stride = sizeof(WideExit);
    wide.arrivals = &wide_ins[0].arrival;
    wide.arrival_stride = sizeof(WideArrival);
    wide.spots = &wide_things[0].spot;
    wide.spot_stride = sizeof(WideSpot);
    wide.cart = &wide_cart.arrival;
    check(service != nullptr && service->add_area(mod, &wide) == 0 && maps::play_quest_in("wide_camp", 62706u) &&
            maps::play_quest_in("yard", 62706u),
        "points: an area whose exits, arrivals, spots and cart are wider than this header's is added");
    memory.store16(0x09000000u + 0x60472u, 62706u);
    check(entry(served(0x1262u), 5) == camp_points,
        "points: its wider elements are read by their strides, their extra bytes ignored: the same points table");
    // Past the map's areas, nothing of the quest's is served: its blocks and
    // table would land on other entries (a ground block read as a table).
    memory.store16(0x09000000u + 0x60472u, 62701u);
    check(maps::play_quest_in("camp", 62701u), "points: another quest in the camp");
    memory.store8(0x0A0457ECu + 8u, 0u);
    check(maps::serve(runtime, 0x1262u) == nullptr && maps::serve(runtime, 0x138Au) == nullptr &&
            maps::serve(runtime, 0x1389u) == nullptr,
        "points: when the map has no area for the camp, none of its files is served");
    memory.store8(0x0A0457ECu + 8u, 9u);
    check(maps::serve(runtime, 0x1262u) == nullptr && maps::serve(runtime, 0x138Au) == nullptr,
        "points: nor when the map's collision pack has no table for each of its areas");
    memory.store8(0x0A0457ECu + 8u, 2u);
    check(served(0x138Au) == bytes("CAMP GE") && entry(served(0x1262u), 5) != bytes("CAMP POINTS"),
        "points: and with the map's two areas, all of them is");
    check(maps::add_area(runtime, {"third", 8, 2, pmo, bytes("g"), bytes(".TMH"), bytes("4.00"), bytes("4.00")}) &&
            maps::play_quest_in("camp", 62703u) && maps::play_quest_in("third", 62703u),
        "points: an area of the arena past its two, giving no points");
    memory.store16(0x09000000u + 0x60472u, 62703u);
    check(maps::serve(runtime, 0x1262u) == nullptr && maps::serve(runtime, 0x138Au) == nullptr &&
            maps::serve(runtime, 0x1389u) == nullptr,
        "points: a quest with an area past the map's, even one giving no points, has none of its areas served");
    // An exit or an arrival naming an area past the map's.
    maps::Points far_exit;
    far_exit.exits = {{0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 5u, 0u}};
    maps::Points far_arrival;
    far_arrival.arrivals = {{0.0f, 0.0f, 0.0f, 0u, 3u, 0u}};
    check(maps::add_area(runtime,
              {"far_exit", 8, 1, pmo, bytes("g"), bytes(".TMH"), bytes("4.00"), bytes("4.00"), "P", far_exit}) &&
            maps::add_area(runtime,
                {"far_arrival", 8, 1, pmo, bytes("g"), bytes(".TMH"), bytes("4.00"), bytes("4.00"), "P",
                    far_arrival}) &&
            maps::play_quest_in("camp", 62704u) && maps::play_quest_in("far_exit", 62704u) &&
            maps::play_quest_in("far_arrival", 62705u),
        "points: areas whose exit leads, or arrival comes from, past the map's areas");
    memory.store16(0x09000000u + 0x60472u, 62704u);
    const bool exit_refused = maps::serve(runtime, 0x138Au) == nullptr && maps::serve(runtime, 0x1262u) == nullptr;
    memory.store16(0x09000000u + 0x60472u, 62705u);
    check(exit_refused && maps::serve(runtime, 0x138Cu) == nullptr && maps::serve(runtime, 0x1262u) == nullptr,
        "points: such a quest has none of its areas served");
    // All or nothing: one area too large to fit, and none of the quest's is served.
    check(maps::add_area(
              runtime, {"huge", 8, 1, pmo, Bytes(0x2000u, 1u), bytes(".TMH"), bytes("4.00"), bytes("4.00"), "P"}) &&
            maps::play_quest_in("camp2", 62702u) && maps::play_quest_in("huge", 62702u),
        "points: a quest in a camp and an area too large");
    memory.store16(0x09000000u + 0x60472u, 62702u);
    check(maps::serve(runtime, 0x1389u) == nullptr && maps::serve(runtime, 0x138Au) == nullptr &&
            maps::serve(runtime, 0x1262u) == nullptr,
        "points: when one of a quest's areas does not fit, none of them is served");

    // Quests that name several areas.
    const auto *quests = static_cast<const ymod_quests *>(mhp3rd::mods::native::host()->service(mod, YMOD_QUESTS, 1u));
    const ymod_quest_monster pair[] = {
        {"tigrex", 0, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100}, {"tigrex", 1, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100}};
    const char *both[] = {"camp", "yard"};
    ymod_quest quest{sizeof(ymod_quest), 62710u, 300, 1, 30u, 0u, 0u, "tigrex", 1u, pair, 2u, {}};
    quest.areas = both;
    quest.area_count = 2u;
    const auto built_quest = [&](std::uint32_t id) -> std::optional<Bytes> {
        const auto file = mhp3rd::mods::p3rd::quest_file(id);
        if (!file) return std::nullopt;
        runtime.cpu().gpr[5] = *file;
        const std::uint32_t size = call(runtime, 0x08864E8Cu, 0x08B3F040u);
        runtime.cpu().gpr[5] = *file;
        runtime.cpu().gpr[6] = 0x08C00000u;
        (void)call(runtime, 0x08863CDCu, 0x08B3F040u);
        Bytes encrypted(size);
        memory.copy_out(0x08C00000u, encrypted);
        game::GuestRam keys_ram(memory);
        if (const auto keys = game::read_quest_keys(keys_ram)) return game::decrypt_quest(encrypted, *keys);
        return std::nullopt;
    };
    const auto area_of = [](const Bytes &b, std::size_t monster) {
        const std::uint32_t spawns = word_at(b, word_at(b, 0x20u) + 12u);
        return static_cast<std::uint32_t>(b[spawns + 0x30u * monster + 6u] | (b[spawns + 0x30u * monster + 7u] << 8u));
    };
    std::optional<Bytes> built =
        quests != nullptr && quests->add(mod, &quest) == 62710 ? built_quest(62710u) : std::nullopt;
    std::optional<game::QuestRecord> record = built ? game::read_quest_record(*built) : std::nullopt;
    memory.store16(0x09000000u + 0x60472u, 62710u);
    check(built && record && record->map == 8u && area_of(*built, 0) == 0u && area_of(*built, 1) == 1u &&
            served(0x138Au) == bytes("CAMP GE") && served(0x138Cu) == bytes("YARD GE"),
        "points: a quest in two areas is on their map, its monsters in the areas they name, both areas served");
    // With `area` too: its monsters of area 0 start in it.
    const char *camp_only[] = {"camp"};
    ymod_quest with_area = quest;
    with_area.id = 62711u;
    with_area.area = "yard";
    with_area.areas = camp_only;
    with_area.area_count = 1u;
    built = quests != nullptr && quests->add(mod, &with_area) == 62711 ? built_quest(62711u) : std::nullopt;
    check(built && area_of(*built, 0) == 1u && area_of(*built, 1) == 1u,
        "points: with `area` and `areas`, a monster of area 0 starts in `area`");
    const auto refuse_quest = [&](std::uint32_t id, std::vector<const char *> ids, const std::string &line) {
        ymod_quest q = quest;
        q.id = id;
        q.areas = ids.data();
        q.area_count = static_cast<std::uint32_t>(ids.size());
        return quests != nullptr && quests->add(mod, &q) == -1 &&
            logged("quest " + std::to_string(id) + " is not added: " + line);
    };
    check(refuse_quest(62712u, {"camp", "nowhere"}, "its area \"nowhere\" has not been added") &&
            refuse_quest(62713u, {"camp", "elsewhere"}, "its areas camp and elsewhere are on different maps") &&
            refuse_quest(62714u, {"camp", "camp2"}, "its areas camp and camp2 take the place of the same area") &&
            refuse_quest(62715u, {"camp", "camp"}, "it names its area \"camp\" twice"),
        "points: a quest naming an area not added, areas on two maps, two in place of one area or one twice is "
        "refused");
    std::vector<const char *> fourteen(14u, "camp");
    check(refuse_quest(62717u, fourteen, "it names more than 13 areas"),
        "points: a quest naming more areas than a map has is refused before they are read");
    // A quest struct that ends before its monsters' stride cannot give them,
    // and is refused with the reason: the same quest with the full struct
    // (62710) is played in the camp and the yard.
    ymod_quest older = quest;
    older.size = offsetof(ymod_quest, areas);
    older.id = 62716u;
    older.map = 8;
    const bool older_refused = quests != nullptr && quests->add(mod, &older) == -1 &&
        mhp3rd::mods::quests::find(62716u) == nullptr &&
        logged("a ymod_quest of " + std::to_string(offsetof(ymod_quest, areas)) +
            " bytes is refused: this call reads " +
            std::to_string(offsetof(ymod_quest, monster_stride) + sizeof(std::uint32_t)) +
            " (set its size to sizeof(ymod_quest))");
    memory.store16(0x09000000u + 0x60472u, 62710u);
    check(older_refused && maps::serve(runtime, 0x1262u) != nullptr,
        "points: a quest struct too short for its monsters' stride is refused, with the reason");

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
}

// Gathering points, ballistas and cannons: their records' words, what a
// gathering point gives, and the quests played in such an area, whose lists
// are built empty first and again with the game's item names.
void test_map_gathering(psprecomp::Runtime &runtime) {
    namespace maps = mhp3rd::mods::maps;
    namespace game = mhp3rd::game;
    using Bytes = std::vector<std::uint8_t>;
    const auto u16 = [](const Bytes &b, std::size_t at) {
        return static_cast<std::uint16_t>(b[at] | (b[at + 1u] << 8u));
    };

    // The word (+28): a gathering point's list from 1, a ballista's or a
    // cannon's number among its kind, 0 for the camp's things; checked
    // against the disc's sand ship (map 6, area 1: gathering 1 and 2,
    // ballistas 0 and 1, cannons 0 and 1).
    maps::Points points;
    const maps::Gathering pile{{{"Cannon Ammo", 1u, 100u}}, 10u, 0u};
    points.spots = {{maps::SpotKind::Gathering, 0.0f, 400.0f, 1306.0f, 265.0f, 200.0f, 0u, pile},
        {maps::SpotKind::Cannon, -257.0f, 250.0f, 415.0f, 150.0f, 150.0f, 0xC000u},
        {maps::SpotKind::Ballista, -280.0f, 243.0f, -124.0f, 200.0f, 200.0f, 0xC000u},
        {maps::SpotKind::Gathering, 33.0f, 215.0f, -860.0f, 200.0f, 100.0f, 0u, pile},
        {maps::SpotKind::Cannon, 257.0f, 250.0f, 415.0f, 150.0f, 150.0f, 0x4000u},
        {maps::SpotKind::Ballista, 280.0f, 243.0f, -124.0f, 200.0f, 200.0f, 0x4000u},
        {maps::SpotKind::Bed, 0.0f, 0.0f, 0.0f, 130.0f, 300.0f, 0u}};
    const Bytes table = maps::write_points(points);
    const auto record = [&table](std::size_t i) { return 0x10u + 32u * i; };
    std::array<std::uint16_t, 7> words{}, kinds{};
    for (std::size_t i = 0; i < 7u; ++i) {
        words[i] = u16(table, record(i) + 28u);
        kinds[i] = u16(table, record(i) + 30u);
    }
    check(word_at(table, 4u) == 2u && word_at(table, 8u) == 7u &&
            words == std::array<std::uint16_t, 7>{1, 0, 0, 2, 1, 1, 0} &&
            kinds == std::array<std::uint16_t, 7>{2, 10, 9, 2, 10, 9, 7} && u16(table, record(1) + 24u) == 0xC000u &&
            u16(table, record(4) + 26u) == 4u,
        "gathering: gathering points take their lists from 1, ballistas and cannons their numbers among their kind, "
        "as the disc's sand ship");

    // Through the service.
    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
    loader_stand_ins(runtime);
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "yakumo_map_gathering_test";
    std::filesystem::create_directories(folder);
    const auto write = [&folder](const char *name, std::string_view text) {
        std::ofstream(folder / name, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
    };
    write("model.pmo",
        std::string_view("pmo\0"
                         "102\0YARD",
            12));
    write("ge.bin", "YARD GE");
    write("tex.tmh", ".TMH yard");
    write("ground.bin", "4.00 ground");
    write("walls.bin", "4.00 walls");
    ymod_mod *mod = mhp3rd::mods::native::add_mod("test.gathering", "Gathering test", folder.string());
    const auto *service = static_cast<const ymod_maps *>(mhp3rd::mods::native::host()->service(mod, YMOD_MAPS, 1u));
    const ymod_area_item balls[] = {{"Cannon Ammo", 1u, 100u}};
    const ymod_area_item bolts[] = {{"Ballista Ammo", 2u, 70u}, {"Cannon Ammo", 1u, 30u}};
    const ymod_area_spot things[] = {{YMOD_SPOT_CANNON, {-1869.0f, 503.0f, 769.0f}, 200.0f, 300.0f, 6736},
        {YMOD_SPOT_GATHERING, {-1600.0f, 503.0f, 600.0f}, 150.0f, 200.0f, 0, balls, 1u, 10u, 0u},
        {YMOD_SPOT_BALLISTA, {2496.0f, 350.0f, 609.0f}, 200.0f, 300.0f, 0},
        {YMOD_SPOT_GATHERING, {2300.0f, 350.0f, 609.0f}, 150.0f, 200.0f, 0, bolts, 2u, 3u, 2u}};
    const ymod_area_arrival in{{0.0f, 0.0f, 3300.0f}, 0x8000, 0, 0};
    ymod_area yard{sizeof(ymod_area), "yard", 8, 1, "model.pmo", "ge.bin", "tex.tmh", "ground.bin", "walls.bin"};
    yard.arrivals = &in;
    yard.arrival_count = 1u;
    yard.spots = things;
    yard.spot_count = 4u;
    const std::vector<maps::Gathering> lists = service != nullptr && service->add_area(mod, &yard) == 0
        ? maps::gathering("yard")
        : std::vector<maps::Gathering>{};
    check(lists.size() == 2u && lists[0].items.size() == 1u && lists[0].items[0].item == "Cannon Ammo" &&
            lists[0].uses == 10u && lists[1].items.size() == 2u && lists[1].items[0].item == "Ballista Ammo" &&
            lists[1].items[0].count == 2u && lists[1].items[0].chance == 70u && lists[1].uses == 3u &&
            lists[1].extra_uses == 2u && maps::gathering("nowhere").empty(),
        "gathering: an area with cannons, a ballista and gathering points is added, its points' lists kept in order");

    const auto refused = [&](ymod_area area, const char *id, const std::string &line) {
        area.id = id;
        return service != nullptr && service->add_area(mod, &area) == -1 && maps::find(id) == nullptr &&
            logged("area " + std::string(id) + " is not added: " + line);
    };
    const auto one_spot = [&](const ymod_area_spot &spot) {
        ymod_area a = yard;
        a.spots = &spot;
        a.spot_count = 1u;
        return a;
    };
    const ymod_area_spot pile_spot = things[1];
    ymod_area_spot no_items = pile_spot;
    no_items.items = nullptr;
    ymod_area_spot uncounted = pile_spot;
    uncounted.item_count = 0u;
    ymod_area_spot too_many_items = pile_spot;
    too_many_items.item_count = 17u;
    ymod_area_spot too_many_uses = pile_spot;
    too_many_uses.uses = 100u;
    ymod_area_spot too_many_extra = pile_spot;
    too_many_extra.extra_uses = 100u;
    ymod_area_spot over_99 = pile_spot;
    over_99.uses = 60u;
    over_99.extra_uses = 40u;
    ymod_area_spot unused = pile_spot;
    unused.uses = 0u;
    const ymod_area_item unnamed[] = {{nullptr, 1u, 100u}};
    const ymod_area_item none_given[] = {{"Cannon Ammo", 0u, 100u}};
    const ymod_area_item lots[] = {{"Cannon Ammo", 100u, 100u}};
    const ymod_area_item never[] = {{"Cannon Ammo", 1u, 0u}};
    const ymod_area_item uneven[] = {{"Cannon Ammo", 1u, 60u}};
    const ymod_area_item empty_name[] = {{"", 1u, 100u}};
    std::vector<ymod_area_spot> bad_items;
    for (const ymod_area_item *list : {unnamed, none_given, lots, never}) {
        ymod_area_spot t = pile_spot;
        t.items = list;
        bad_items.push_back(t);
    }
    ymod_area_spot uneven_spot = pile_spot;
    uneven_spot.items = uneven;
    ymod_area_spot empty_name_spot = pile_spot;
    empty_name_spot.items = empty_name;
    ymod_area_spot loaded_cannon = things[0];
    loaded_cannon.items = balls;
    loaded_cannon.item_count = 1u;
    ymod_area_spot used_bed = {YMOD_SPOT_BED, {0.0f, 0.0f, 0.0f}, 130.0f, 300.0f, 0};
    used_bed.uses = 1u;
    ymod_area_spot gong = things[0];
    gong.kind = 12;
    const std::string count_line = "a thing to use counts items it does not give";
    const std::string bound_line = "a thing to use gives more than 16 items or more than 99 uses";
    const std::string item_line = "a gathered item's name, count or chance is out of range";
    const std::string uses_line = "a gathering point's uses are not 1 to 99";
    bool all_refused = refused(one_spot(no_items), "no_items", count_line);
    all_refused =
        refused(one_spot(uncounted), "uncounted", "a gathering point gives no item or more than 16") && all_refused;
    all_refused = refused(one_spot(too_many_items), "too_many_items", bound_line) && all_refused;
    all_refused = refused(one_spot(too_many_uses), "too_many_uses", bound_line) && all_refused;
    all_refused = refused(one_spot(too_many_extra), "too_many_extra", bound_line) && all_refused;
    all_refused = refused(one_spot(over_99), "over_99", uses_line) && all_refused;
    all_refused = refused(one_spot(unused), "unused", uses_line) && all_refused;
    for (std::size_t i = 0; i < bad_items.size(); ++i)
        all_refused = refused(one_spot(bad_items[i]), ("item" + std::to_string(i)).c_str(), item_line) && all_refused;
    all_refused =
        refused(one_spot(uneven_spot), "uneven", "a gathering point's chances do not add up to 100") && all_refused;
    all_refused =
        refused(one_spot(empty_name_spot), "empty_name", "a gathered item is unnamed or none of it is given") &&
        all_refused;
    all_refused =
        refused(one_spot(loaded_cannon), "loaded_cannon", "only a gathering point gives items") && all_refused;
    all_refused = refused(one_spot(used_bed), "used_bed", "only a gathering point gives items") && all_refused;
    all_refused = refused(one_spot(gong), "gong",
                      "a thing to use is not a gathering point, supply box, delivery box, bed, ballista or cannon, "
                      "or its facing is out of range") &&
        all_refused;
    check(all_refused,
        "gathering: items counted but not given, more than 16, more than 99 uses, none or past 99 with the extra "
        "ones, an item unnamed or of none, 100 or with no chance, chances not adding up to 100, items on a cannon "
        "or uses on a bed, and a kind not offered (the gong) are refused");
    // A spot's items by their stride: wider ones, as a later SDK's, read with
    // their extra bytes ignored; a stride below the first SDK's item refused.
    struct WideItem {
        ymod_area_item item;
        const char *later;
    };
    const WideItem wide_bolts[] = {{bolts[0], "not an item"}, {bolts[1], "not an item"}};
    ymod_area_spot wide_pile = things[3];
    wide_pile.items = &wide_bolts[0].item;
    wide_pile.item_stride = sizeof(WideItem);
    ymod_area wide_yard = one_spot(wide_pile);
    wide_yard.id = "wide_yard";
    const std::vector<maps::Gathering> wide_lists = service != nullptr && service->add_area(mod, &wide_yard) == 0
        ? maps::gathering("wide_yard")
        : std::vector<maps::Gathering>{};
    check(wide_lists.size() == 1u && wide_lists[0].items.size() == 2u &&
            wide_lists[0].items[0].item == "Ballista Ammo" && wide_lists[0].items[0].chance == 70u &&
            wide_lists[0].items[1].item == "Cannon Ammo" && wide_lists[0].items[1].count == 1u &&
            wide_lists[0].items[1].chance == 30u,
        "gathering: a point's items wider than this header's are read by their stride, their extra bytes ignored");
    constexpr std::size_t kItemEnd = offsetof(ymod_area_item, chance) + sizeof(std::uint32_t);
    ymod_area_spot narrow_pile = pile_spot;
    narrow_pile.item_stride = static_cast<std::uint32_t>(offsetof(ymod_area_item, chance));
    ymod_area_spot unstrided_pile = pile_spot;
    unstrided_pile.item_stride = 0u;
    const auto item_stride_line = [&](std::size_t stride) {
        return "item_stride is " + std::to_string(stride) + ", less than the " + std::to_string(kItemEnd) +
            " bytes of a ymod_area_item: set it to sizeof(ymod_area_item)";
    };
    check(refused(one_spot(narrow_pile), "narrow_pile", item_stride_line(offsetof(ymod_area_item, chance))) &&
            refused(one_spot(unstrided_pile), "unstrided_pile", item_stride_line(0u)),
        "gathering: a point whose items' stride is below the first SDK's item, or 0, is refused with the reason");
    ymod_area uncounted_exits = yard;
    uncounted_exits.exit_count = 1u;
    ymod_area uncounted_arrivals = yard;
    uncounted_arrivals.arrivals = nullptr;
    ymod_area uncounted_spots = yard;
    uncounted_spots.spots = nullptr;
    const std::string array_line = "it counts exits, arrivals or things to use it does not give";
    check(refused(uncounted_exits, "uncounted_exits", array_line) &&
            refused(uncounted_arrivals, "uncounted_arrivals", array_line) &&
            refused(uncounted_spots, "uncounted_spots", array_line),
        "gathering: an area counting exits, arrivals or things it gives no array for is refused");

    // A quest played in the yard: its gathering lists in area 1, empty until
    // the game's item names are known, then named.
    const auto *quests = static_cast<const ymod_quests *>(mhp3rd::mods::native::host()->service(mod, YMOD_QUESTS, 1u));
    const ymod_quest_monster hunted{"tigrex", 0, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100};
    ymod_quest quest{sizeof(ymod_quest), 62720u, 8, 1, 30u, 0u, 0u, "tigrex", 1u, &hunted, 1u, {}};
    quest.area = "yard";
    auto &memory = runtime.memory();
    const auto built_quest = [&](std::uint32_t id) -> std::optional<Bytes> {
        const auto file = mhp3rd::mods::p3rd::quest_file(id);
        if (!file) return std::nullopt;
        runtime.cpu().gpr[5] = *file;
        const std::uint32_t size = call(runtime, 0x08864E8Cu, 0x08B3F040u);
        runtime.cpu().gpr[5] = *file;
        runtime.cpu().gpr[6] = 0x08C00000u;
        (void)call(runtime, 0x08863CDCu, 0x08B3F040u);
        Bytes encrypted(size);
        memory.copy_out(0x08C00000u, encrypted);
        game::GuestRam keys_ram(memory);
        if (const auto keys = game::read_quest_keys(keys_ram)) return game::decrypt_quest(encrypted, *keys);
        return std::nullopt;
    };
    // Area 1's entries: the offset to the point's items, its most and fewest uses.
    const auto entry_of = [](const Bytes &q, std::size_t area, std::size_t point) {
        return word_at(q, word_at(q, 0x60u) + 4u * area) + 8u * point;
    };
    ymod_quest misnamed = quest;
    misnamed.id = 62721u;
    std::optional<Bytes> first =
        quests != nullptr && quests->add(mod, &quest) == 62720 ? built_quest(62720u) : std::nullopt;
    check(first && word_at(*first, entry_of(*first, 1u, 0u)) == 0u &&
            u16(*first, entry_of(*first, 1u, 0u) + 4u) == 10u && u16(*first, entry_of(*first, 1u, 1u) + 4u) == 5u &&
            u16(*first, entry_of(*first, 1u, 1u) + 6u) == 3u && word_at(*first, entry_of(*first, 0u, 0u)) == 0u,
        "gathering: a quest in the area is first built with its points' uses and no items");
    // The text block's item names (table 3): 0 a placeholder, 1 and 2.
    constexpr std::uint32_t kBlock = 0x08A40640u, kNames = kBlock + 0x3000u;
    memory.store32(kBlock + 3u * 4u, 0x3000u);
    memory.store32(kNames, 16u);
    memory.store32(kNames + 4u, 18u);
    memory.store32(kNames + 8u, 30u);
    memory.store32(kNames + 12u, 0xFFFFFFFFu);
    for (const auto &[at, text] :
        {std::pair<std::uint32_t, const char *>{16u, "-"}, {18u, "Cannon Ammo"}, {30u, "Ballista Ammo"}}) {
        std::uint32_t i = 0;
        for (; text[i] != '\0'; ++i) memory.store8(kNames + at + i, static_cast<std::uint8_t>(text[i]));
        memory.store8(kNames + at + i, 0u);
    }
    // A second area, one of whose two points names an item the game does not
    // have, and a quest there with a reward.
    const ymod_area_item odd[] = {{"Unknown Thing", 1u, 100u}};
    ymod_area_spot odd_piles[] = {pile_spot, pile_spot};
    odd_piles[0].items = odd;
    ymod_area odd_yard = one_spot(odd_piles[0]);
    odd_yard.spots = odd_piles;
    odd_yard.spot_count = 2u;
    odd_yard.id = "odd_yard";
    misnamed.area = "odd_yard";
    const ymod_quest_reward prize[] = {{"Ballista Ammo", 1u, 100u, 0u}};
    misnamed.rewards = prize;
    misnamed.reward_count = 1u;
    check(service != nullptr && service->add_area(mod, &odd_yard) == 0 && quests->add(mod, &misnamed) == 62721,
        "gathering: an area whose point names an item the game lacks, and a quest in it, are added");
    const std::optional<Bytes> misnamed_first = built_quest(62721u);
    place_overlay(runtime, 0x0A05E600u, "lobby_task.ovl", 0x00200000u);
    mhp3rd::mods::quests::overlay_loaded(runtime);
    const std::optional<Bytes> named = built_quest(62720u);
    const std::size_t pile_items = named ? word_at(*named, entry_of(*named, 1u, 0u)) : 0u;
    const std::size_t bolt_items = named ? word_at(*named, entry_of(*named, 1u, 1u)) : 0u;
    check(named && pile_items != 0u && (*named)[pile_items] == 100u && (*named)[pile_items + 1u] == 1u &&
            u16(*named, pile_items + 2u) == 1u && word_at(*named, pile_items + 4u) == 0xFFFFFFFFu &&
            (*named)[bolt_items] == 70u && (*named)[bolt_items + 1u] == 2u && u16(*named, bolt_items + 2u) == 2u &&
            (*named)[bolt_items + 4u] == 30u && u16(*named, bolt_items + 6u) == 1u &&
            u16(*named, entry_of(*named, 1u, 0u) + 4u) == 10u,
        "gathering: once lobby_task loads, the quest is built again with its points' items named");
    const std::optional<Bytes> misnamed_again = built_quest(62721u);
    const std::size_t kept_items = misnamed_again ? word_at(*misnamed_again, entry_of(*misnamed_again, 1u, 1u)) : 0u;
    const std::size_t prize_items =
        misnamed_again ? word_at(*misnamed_again, word_at(*misnamed_again, 0x1Cu) + 8u) : 0u;
    check(misnamed_first && misnamed_again && *misnamed_first != *misnamed_again &&
            word_at(*misnamed_again, entry_of(*misnamed_again, 1u, 0u)) == 0u && kept_items != 0u &&
            u16(*misnamed_again, kept_items + 2u) == 1u && u16(*misnamed_again, prize_items + 2u) == 2u &&
            logged("quest 62721: a gathering point of area 1 names no item of the game's, \"Unknown Thing\"; it "
                   "gives nothing"),
        "gathering: a point naming no item of the game's gives nothing, and is reported; the quest's other point "
        "and its reward are built as named");

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
}

// The quest's three ballista slots, the areas' conditional words and the
// areas a map can have.
void test_map_slots(psprecomp::Runtime &runtime) {
    namespace maps = mhp3rd::mods::maps;
    namespace game = mhp3rd::game;
    using Bytes = std::vector<std::uint8_t>;
    using Words = std::vector<std::uint16_t>;
    const auto bytes = [](std::string_view s) { return Bytes(s.begin(), s.end()); };
    const auto u16 = [](const Bytes &b, std::size_t at) {
        return static_cast<std::uint16_t>(b[at] | (b[at + 1u] << 8u));
    };
    const auto entry = [](const Bytes &pack, std::size_t i) {
        const auto entries = game::read_pack(pack);
        return entries && i < entries->size() ? Bytes((*entries)[i].begin(), (*entries)[i].end()) : Bytes{};
    };

    // game_sub's six conditional rows (0x0A044A58): map 12's area 4 makes
    // word 2 so, map 3's area 4 word 5; other areas have none.
    check(maps::point_words(12u, 4u, 3u, 1u) == Words{1, 3, 4} &&
            maps::point_words(12u, 4u, 3u, 0u) == Words{0, 1, 3} && maps::point_words(19u, 5u, 2u, 3u) == Words{4, 5} &&
            maps::point_words(3u, 4u, 6u, 0u) == Words{0, 1, 2, 3, 4, 6} &&
            maps::point_words(8u, 1u, 3u, 0u) == Words{0, 1, 2} && maps::point_words(12u, 4u, 0u, 0u).empty(),
        "slots: an area's conditional words are passed over, and only that area's");
    maps::Points points;
    points.spots = {
        {maps::SpotKind::Gathering, 0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0u, {{{"Cannon Ammo", 1u, 100u}}, 1u, 0u}},
        {maps::SpotKind::Cannon, 0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0u},
        {maps::SpotKind::Ballista, 0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0u},
        {maps::SpotKind::Gathering, 0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0u, {{{"Cannon Ammo", 1u, 100u}}, 1u, 0u}},
        {maps::SpotKind::Cannon, 0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0u},
        {maps::SpotKind::Cannon, 0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0u},
        {maps::SpotKind::Ballista, 0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0u}};
    const auto words_of = [&u16](const Bytes &table) {
        Words out;
        for (std::size_t i = 0; i < word_at(table, 8u); ++i) out.push_back(u16(table, 0x10u + 32u * i + 28u));
        return out;
    };
    check(words_of(maps::write_points(points, {12u, 4u, 0u})) == Words{1, 0, 0, 3, 1, 3, 1} &&
            words_of(maps::write_points(points, {8u, 1u, 1u})) == Words{1, 0, 1, 2, 1, 2, 2},
        "slots: an area's points are numbered past its conditional words, its ballistas from the quest's next slot");

    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
    loader_stand_ins(runtime);
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "yakumo_map_slots_test";
    std::filesystem::create_directories(folder);
    const auto write = [&folder](const char *name, std::string_view text) {
        std::ofstream(folder / name, std::ios::binary).write(text.data(), static_cast<std::streamsize>(text.size()));
    };
    write("model.pmo",
        std::string_view("pmo\0"
                         "102\0SLOT",
            12));
    write("ge.bin", "GE");
    write("tex.tmh", ".TMH");
    write("ground.bin", "4.00 ground");
    write("walls.bin", "4.00 walls");
    ymod_mod *mod = mhp3rd::mods::native::add_mod("test.slots", "Slots test", folder.string());
    const auto *service = static_cast<const ymod_maps *>(mhp3rd::mods::native::host()->service(mod, YMOD_MAPS, 1u));
    const ymod_area_item balls[] = {{"Cannon Ammo", 1u, 100u}};
    const ymod_area_spot ballista{YMOD_SPOT_BALLISTA, {0.0f, 0.0f, 0.0f}, 200.0f, 300.0f, 0};
    const ymod_area_spot pile{YMOD_SPOT_GATHERING, {0.0f, 0.0f, 0.0f}, 150.0f, 200.0f, 0, balls, 1u, 5u, 0u};
    const ymod_area_arrival in{{0.0f, 0.0f, 0.0f}, 0, YMOD_AREA_QUEST_START, 0};
    const auto area_with = [&](const char *id, int map, int number, const std::vector<ymod_area_spot> &spots) {
        ymod_area a{sizeof(ymod_area), id, map, number, "model.pmo", "ge.bin", "tex.tmh", "ground.bin", "walls.bin"};
        a.arrivals = &in;
        a.arrival_count = 1u;
        a.spots = spots.data();
        a.spot_count = static_cast<std::uint32_t>(spots.size());
        return service != nullptr ? service->add_area(mod, &a) : -1;
    };
    const std::vector<ymod_area_spot> three(3u, ballista), four(4u, ballista), two(2u, ballista);
    check(area_with("three", 8, 1, three) == 0 && area_with("four", 8, 1, four) == -1 &&
            logged("area four is not added: it has more ballistas than the game's 3 for a quest (past its area's "
                   "conditional words)") &&
            area_with("peak_three", 12, 4, three) == -1 && area_with("peak_two", 12, 4, two) == 0,
        "slots: an area with more ballistas than the quest's three slots is refused, and with three where its "
        "conditional word leaves two");
    check(area_with("thirteen", 8, 13, {}) == -1 &&
            logged("area thirteen is not added: area 13 is past the 13 a map can "
                   "have") &&
            area_with("twelve", 8, 12, {}) == 0,
        "slots: an area past the 13 a map can have is refused; the 13th is added");
    const std::vector<ymod_area_spot> piles(2u, pile);
    const std::vector<maps::Gathering> lists =
        area_with("peak_piles", 12, 4, piles) == 0 ? maps::gathering("peak_piles") : std::vector<maps::Gathering>{};
    check(lists.size() == 3u && lists[0].items.size() == 1u && lists[1].items.empty() && lists[1].uses == 0u &&
            lists[2].items.size() == 1u,
        "slots: the gathering lists of an area with a conditional word keep an empty one at it");

    // Across a quest's areas: two in the camp and two on the floor need four.
    const std::vector<ymod_area_spot> camp_two(2u, ballista), floor_one(1u, ballista);
    check(area_with("camp_two", 8, 0, camp_two) == 0 && area_with("floor_two", 8, 1, two) == 0 &&
            area_with("floor_one", 8, 1, floor_one) == 0,
        "slots: areas with two, two and one ballistas are added");
    check(maps::quest_points_problem({"camp_two", "floor_two"}) &&
            !maps::quest_points_problem({"camp_two", "floor_one"}) && !maps::quest_points_problem({"three", "nowhere"}),
        "slots: a quest's ballistas are counted across its areas");
    const auto *quests = static_cast<const ymod_quests *>(mhp3rd::mods::native::host()->service(mod, YMOD_QUESTS, 1u));
    const ymod_quest_monster hunted{"tigrex", 1, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100};
    const char *too_many[] = {"camp_two", "floor_two"};
    ymod_quest crowded{sizeof(ymod_quest), 62730u, 8, 1, 30u, 0u, 0u, "tigrex", 1u, &hunted, 1u, {}};
    crowded.areas = too_many;
    crowded.area_count = 2u;
    std::string why;
    check(quests != nullptr && quests->add(mod, &crowded) == -1 &&
            logged("quest 62730 is not added: its areas have more ballistas than the game's 3 for a quest "
                   "(numbered across them, past their areas' conditional words)") &&
            maps::play_quest_in("camp_two", 62731u) && !maps::play_quest_in("floor_two", 62731u, &why) &&
            why.find("more ballistas") != std::string::npos,
        "slots: a quest whose areas have four ballistas is refused, and so is playing a quest in them");

    // Served: the camp's two ballistas take slots 0 and 1, the floor's 2.
    const char *fitting[] = {"camp_two", "floor_one"};
    ymod_quest fits = crowded;
    fits.id = 62732u;
    fits.areas = fitting;
    maps::read_files_with_for_tests([&](mhp3rd::mods::FileId file) -> std::optional<Bytes> {
        if (file == 0x1389u || file == 0x138Bu) return game::write_pack({bytes("M"), bytes("S"), {}, bytes("T")});
        if (file == 0x138Au || file == 0x138Cu) return Bytes(0x1000u, 0u);
        if (file == 0x1262u)
            return game::write_pack(
                {bytes("TMH"), bytes("C1"), bytes("C2"), bytes("F1"), bytes("F2"), bytes("CP"), bytes("FP")});
        return std::nullopt;
    });
    auto &memory = runtime.memory();
    place_overlay(runtime, 0x0A001780u, "game_sub.ovl", 0x00050000u);
    memory.store16(0x0A0457C4u + 16u, 0x1389u);
    memory.store16(0x0A04579Cu + 16u, 0x138Au);
    memory.store8(0x0A0457ECu + 8u, 2u);
    memory.store32(0x08AB3640u, 0x09000000u);
    memory.store16(0x09000000u + 0x60472u, 62732u);
    const bool fits_added = quests != nullptr && quests->add(mod, &fits) == 62732;
    const auto served = maps::serve(runtime, 0x1262u);
    const Bytes camp_table = served ? entry(*served, 5) : Bytes{};
    const Bytes floor_table = served ? entry(*served, 6) : Bytes{};
    // Each table's things to use (group 2): their words.
    const auto spot_words = [&u16](const Bytes &table) {
        Words out;
        for (std::size_t g = 0; g < word_at(table, 0u); ++g) {
            if (word_at(table, 4u + 12u * g) != 2u) continue;
            for (std::size_t i = 0; i < word_at(table, 8u + 12u * g); ++i)
                out.push_back(u16(table, word_at(table, 12u + 12u * g) + 32u * i + 28u));
        }
        return out;
    };
    check(fits_added && served && spot_words(camp_table) == Words{0, 1} && spot_words(floor_table) == Words{2},
        "slots: a quest's ballistas are numbered across its areas, in the order it names them");

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
}

void test_monsters(psprecomp::Runtime &runtime) {
    namespace monsters = mhp3rd::mods::monsters;
    namespace tables = mhp3rd::mods::tables;
    namespace added = mhp3rd::mods::added;
    tables::reset_for_tests();
    monsters::reset_for_tests();
    added::reset_for_tests();

    check(monsters::kind("tigrex") == 58u && monsters::kind("brute_tigrex") == 23u &&
            monsters::kind("rock_peaks") == 74u && !monsters::kind("fatalis") &&
            monsters::id_of(15u) == "gold_rathian" && monsters::id_of(200u).empty() &&
            monsters::game_kinds().size() == 74u,
        "monsters: the game's kinds by id, and back");

    // The model pack: the mod's entries over the base's, and the base's mesh count.
    const std::vector<std::uint8_t> mine = pack_of({{'S', 'K', 'E', 'L'}, small_pmo(2u), {'T', 'E', 'X', '0'}});
    const std::vector<std::uint8_t> base = pack_of({{'B', 'S', 'K', 'L'}, small_pmo(16u), {'B', 'T', 'E', 'X'},
        {'E', '3'}, {}, {'E', '5'}, {'E', '6'}, {'E', '7'}});
    const std::vector<std::uint8_t> composed = monsters::compose_pack(mine, base, 4u);
    check(composed.size() > 8u && word_at(composed, 0u) == 8u &&
            entry_of(composed, 0u) == std::vector<std::uint8_t>{'S', 'K', 'E', 'L'} &&
            entry_of(composed, 2u) == std::vector<std::uint8_t>{'T', 'E', 'X', '0'} &&
            entry_of(composed, 3u) == std::vector<std::uint8_t>{'E', '3'} && entry_of(composed, 4u).empty() &&
            entry_of(composed, 7u) == std::vector<std::uint8_t>{'E', '7'},
        "monsters: the pack keeps the mod's entries and takes the rest from the base");
    const std::vector<std::uint8_t> model = composed.empty() ? std::vector<std::uint8_t>{} : entry_of(composed, 1u);
    const std::vector<std::uint8_t> before = small_pmo(2u);
    check(model.size() == before.size() + 2u * 0x30u && model[0x1Cu] == 4u &&
            word_at(model, 0x24u) == word_at(before, 0x24u) + 0x60u &&
            word_at(model, 0x30u) == word_at(before, 0x30u) + 0x60u && word_at(model, 0x34u) == model.size() &&
            word_at(model, 0x08u) == model.size() + 100u && word_at(model, 0x40u + 0x30u * 3u + 0x20u) == 0x80000003u &&
            word_at(model, 0x40u + 0x30u * 3u + 0x28u) == 0u &&
            word_at(model, 0x40u + 0x30u * 1u + 0x20u) == 0x80000007u,
        "monsters: the model gets empty meshes up to the base's count, and the tables behind them move");
    check(monsters::compose_pack(mine, base, 2u).size() ==
            pack_of({entry_of(mine, 0u), small_pmo(2u), entry_of(mine, 2u), {'E', '3'}, {}, {'E', '5'}, {'E', '6'},
                        {'E', '7'}})
                .size(),
        "monsters: a model with enough meshes is left as it is");
    check(monsters::compose_pack({}, base, 4u).empty() &&
            monsters::compose_pack(mine, std::vector<std::uint8_t>(3u, 0u), 4u).empty(),
        "monsters: a pack that cannot be read gives nothing");
    // Effects name the kind that owns them: two effects, the first none (its
    // offset at the entry's end), the second with one emitter.
    const auto effects_entry = [](std::uint8_t owner) {
        std::vector<std::uint8_t> e(112u, 0u);
        const auto put = [&e](std::size_t at, std::uint32_t v) { std::memcpy(e.data() + at, &v, 4u); };
        put(0u, 2u);
        put(4u, 112u);
        put(8u, 12u);
        put(12u, 1u);
        put(16u, 12u);
        put(20u, 100u);
        put(24u, 0x3F800000u);
        put(28u, 1u);
        e[32u] = 1u;
        e[33u] = owner;
        return e;
    };
    const std::vector<std::uint8_t> fiery = pack_of({{'B', 'S', 'K', 'L'}, small_pmo(16u), {'B', 'T', 'E', 'X'},
        {'E', '3'}, {}, {'E', '5'}, {'E', '6'}, effects_entry(58u)});
    const std::vector<std::uint8_t> other =
        pack_of({{'R'}, small_pmo(2u), {'R'}, {'R', '3'}, {}, {'R', '5'}, {'R', '6'}, effects_entry(2u)});
    const std::vector<std::uint8_t> kept = monsters::compose_pack(mine, fiery, 0u, {}, 58u, 75u);
    const std::vector<std::uint8_t> taken = monsters::compose_pack(mine, fiery, 0u, other, 2u, 75u);
    check(entry_of(kept, 7u).size() == 112u && entry_of(kept, 7u)[33] == 75u && entry_of(kept, 3u)[1] == '3' &&
            entry_of(taken, 3u) == std::vector<std::uint8_t>{'R', '3'} && entry_of(taken, 7u)[33] == 75u &&
            entry_of(taken, 0u) == std::vector<std::uint8_t>{'S', 'K', 'E', 'L'} &&
            entry_of(monsters::compose_pack(mine, fiery, 0u, {}, 2u, 75u), 7u)[33] == 58u,
        "monsters: effects owned by the base or another kind become the new kind's, and can come from another kind");

    std::string why;
    check(!monsters::add(runtime, {"", "tigrex", mine, {1u}, {1u}, "test"}, &why) &&
            why.find("taken or empty") != std::string::npos,
        "monsters: an empty id is refused");
    check(!monsters::add(runtime, {"tigrex", "rathian", mine, {1u}, {1u}, "test"}, &why) &&
            why.find("taken") != std::string::npos,
        "monsters: a game kind's id is refused");
    check(!monsters::add(runtime, {"fatalis", "kirin", mine, {1u}, {1u}, "test"}, &why) &&
            why.find("not one of the game's kinds") != std::string::npos,
        "monsters: an unknown base is refused");

    // The game as add() finds it: the loader, the tables' code and the Tigrex's
    // model set (58, file 5498).
    loader_stand_ins(runtime);
    const auto traced_code = [&runtime] {
        for (const char *name : {"monster.models", "monster.models.second", "monster.models.third", "monster.kinds",
                 "monster.files", "monster.constructors", "monster.sizes", "monster.carves"})
            tables::write_traced_code_for_tests(runtime, name);
    };
    traced_code();
    for (std::uint32_t set = 1u; set <= 74u; ++set)
        runtime.memory().store16(0x08982DD0u + (set - 1u) * 2u, 5000u + set);
    runtime.memory().store16(0x08982DD0u + 57u * 2u, 5498u);
    constexpr std::uint32_t kSlot = 0x0A05E600u;
    place_overlay(runtime, kSlot, "lobby_task.ovl", 0x00200000u);
    monsters::read_packs_with_for_tests([&base](mhp3rd::mods::FileId file) -> std::optional<std::vector<std::uint8_t>> {
        if (file != 5498u) return std::nullopt;
        return base;
    });
    runtime.memory().store32(0x0889EEC4u, 0x91030003u); // the draw's alpha read, as traced
    runtime.memory().store32(0x08A21A40u + 58u * 4u, std::bit_cast<std::uint32_t>(1.5f)); // the Tigrex's step dust
    mhp3rd::mods::native::attach(runtime);
    const std::optional<std::uint8_t> fatalis =
        monsters::add(runtime, {"fatalis", "tigrex", mine, {'G', 'E'}, {'M', 'O'}, "test"}, &why);
    check(fatalis == 75u && monsters::kind("fatalis") == 75u && monsters::id_of(75u) == "fatalis",
        "monsters: a kind is added as 75, and known by its id");
    const std::optional<tables::Placement> sets = tables::placement("monster.models");
    const auto row = [&](const char *table) {
        const std::optional<tables::Placement> at = tables::placement(table);
        return at ? runtime.memory().load16(at->address + 74u * 2u) : 0u;
    };
    check(sets && sets->rows == 75u && row("monster.models") == added::kLastFreeId &&
            row("monster.models.second") == added::kLastFreeId - 1u &&
            row("monster.models.third") == added::kLastFreeId - 2u && added::serves(added::kLastFreeId) &&
            added::serves(added::kLastFreeId - 2u),
        "monsters: its model set points at its three files, which are served");
    check(!monsters::add(runtime, {"fatalis", "tigrex", mine, {1u}, {1u}, "test"}, &why) &&
            why.find("taken") != std::string::npos,
        "monsters: an id already added is refused");
    // Its footsteps' dust: the step scale table (in the executable) a row
    // longer, the base's size in its row; game_sub, which reads it, is pointed
    // at the copy only while it is the overlay loaded there.
    const std::optional<tables::Placement> scales = tables::placement("monster.step_scales");
    check(scales && scales->rows == 76u && scales->address != 0x08A21A40u &&
            runtime.memory().load32(scales->address + 75u * 4u) == std::bit_cast<std::uint32_t>(1.5f) &&
            runtime.memory().load32(scales->address + 58u * 4u) == std::bit_cast<std::uint32_t>(1.5f),
        "monsters: the step scale table grows, and the new kind takes the base's size");
    constexpr std::uint32_t kSubSlot = 0x0A001780u;
    place_overlay(runtime, kSubSlot, "other_sub.ovl", 0x00020000u);
    tables::write_traced_code_for_tests(runtime, "monster.step_scales");
    tables::overlay_loaded(runtime);
    check(runtime.memory().load32(0x0A014710u) == 0x3C0208A2u && runtime.memory().load32(0x0A014714u) == 0x24421A40u,
        "tables: an executable table's reader in an overlay is left alone while another overlay is there");
    place_overlay(runtime, kSubSlot, "game_sub.ovl", 0x00020000u);
    tables::write_traced_code_for_tests(runtime, "monster.step_scales");
    tables::overlay_loaded(runtime);
    const std::uint32_t scale_hi = runtime.memory().load32(0x0A014710u),
                        scale_lo = runtime.memory().load32(0x0A014714u);
    check(scales && (scale_hi & 0xFFFFu) == ((scales->address + 0x8000u) >> 16u) &&
            (scale_lo & 0xFFFFu) == (scales->address & 0xFFFFu) && (scale_hi >> 16u) == 0x3C02u &&
            (scale_lo >> 16u) == 0x2442u,
        "tables: and pointed at the copy once game_sub is loaded there");
    monsters::read_packs_with_for_tests(
        [](mhp3rd::mods::FileId) { return std::optional<std::vector<std::uint8_t>>{}; });
    check(!monsters::add(runtime, {"kirin", "tigrex", mine, {1u}, {1u}, "test"}, &why) &&
            why.find("base's model pack") != std::string::npos,
        "monsters: a base whose model pack cannot be read is refused");
    monsters::read_packs_with_for_tests({});

    // game_task loads: the new kind takes the Tigrex's rows.
    place_overlay(runtime, kSlot, "game_task.ovl", 0x00200000u);
    traced_code();
    for (std::uint32_t at = 0x0A17FE84u; at < 0x0A17FE84u + 75u * 8u; at += 4u) runtime.memory().store32(at, at);
    for (std::uint32_t at = 0x0A19F440u; at < 0x0A19F440u + 75u * 4u; at += 4u) runtime.memory().store32(at, at);
    tables::overlay_loaded(runtime);
    monsters::overlay_loaded(runtime);
    const std::optional<tables::Placement> kinds = tables::placement("monster.kinds");
    const std::optional<tables::Placement> makers = tables::placement("monster.constructors");
    check(kinds && makers && runtime.memory().load32(kinds->address + 75u * 8u) == 0x0A17FE84u + 58u * 8u &&
            runtime.memory().load32(kinds->address + 75u * 8u + 4u) == 0x0A17FE84u + 58u * 8u + 4u &&
            runtime.memory().load32(makers->address + 75u * 4u) == 0x0A19F440u + 58u * 4u,
        "monsters: when game_task loads, the kind takes the base's rows");

    // The Tigrex's behaviour checks its kind: the new kind goes its way.
    place_overlay(runtime, 0x0A1BB000u, "em058m0.ovl", 0x00030000u);
    store(runtime, 0x0A1BB304u, {0x00601021u, 0x03E00008u, 0u}); // addu v0, v1, zero; jr ra
    mhp3rd::mods::native::overlay_loaded(runtime, 0x0A1BB000u);
    runtime.cpu().gpr[3] = 75u;
    const std::uint32_t seen = call(runtime, 0x0A1BB304u, 0u);
    runtime.cpu().gpr[3] = 59u;
    check(seen == 58u && call(runtime, 0x0A1BB304u, 0u) == 59u,
        "monsters: the base's kind check sees the base for the new kind only");
    // A Tigrex as a quest's second monster: its AI, em058m1, in the second
    // slot, with the same check at the same offset.
    place_overlay(runtime, 0x0A1EFE80u, "em058m1.ovl", 0x00030000u);
    store(runtime, 0x0A1F0184u, {0x00601021u, 0x03E00008u, 0u}); // addu v0, v1, zero; jr ra
    mhp3rd::mods::native::overlay_loaded(runtime, 0x0A1EFE80u);
    runtime.cpu().gpr[3] = 75u;
    check(
        call(runtime, 0x0A1F0184u, 0u) == 58u, "monsters: and so does the base's AI loaded in the second monster slot");

    // The Tigrex's behaviour hides materials: the new kind's model keeps them.
    store(runtime, 0x0A08FE8Cu, {0u, 0x03E00008u, 0u});
    store(runtime, 0x0889EEC4u, {0x91030003u, 0x03E00008u, 0u});
    mhp3rd::mods::native::overlay_loaded(runtime, kSlot);
    constexpr std::uint32_t kBuffer = 0x08B10000u, kPmo = 0x08B20000u, kStack = 0x08B30000u;
    runtime.memory().store16(kPmo + 0x1Eu, 4u);
    runtime.memory().store32(kStack + 100u, 75u);
    runtime.memory().store32(kBuffer + 16u, 0x00FFFFFFu);
    runtime.cpu().gpr[6] = kPmo;
    runtime.cpu().gpr[18] = kBuffer;
    runtime.cpu().gpr[29] = kStack;
    (void)call(runtime, 0x0A08FE8Cu, 0u);
    runtime.cpu().gpr[8] = kBuffer + 16u;
    (void)call(runtime, 0x0889EEC4u, 0u);
    check(
        runtime.memory().load32(kBuffer + 16u) == 0xFFFFFFFFu, "monsters: a material its base hid is shown when drawn");
    runtime.memory().store32(kBuffer + 0x100u, 0x00FFFFFFu);
    runtime.cpu().gpr[8] = kBuffer + 0x100u;
    (void)call(runtime, 0x0889EEC4u, 0u);
    check(
        runtime.memory().load32(kBuffer + 0x100u) == 0x00FFFFFFu, "monsters: another model's materials are left alone");
    // An area loading during the quest (game_task still there) keeps the models built.
    mhp3rd::mods::native::frame(runtime); // the quest, seen started
    place_overlay(runtime, 0x0A055E80u, "P_m08a01d.ovl", 0x400u);
    tables::overlay_loaded(runtime);
    monsters::overlay_loaded(runtime);
    runtime.memory().store32(kBuffer + 16u, 0x00FFFFFFu);
    runtime.cpu().gpr[8] = kBuffer + 16u;
    (void)call(runtime, 0x0889EEC4u, 0u);
    check(runtime.memory().load32(kBuffer + 16u) == 0xFFFFFFFFu,
        "monsters: an area loading later keeps its materials shown");
    // The quest ends (game_task leaves): its models are forgotten.
    place_overlay(runtime, kSlot, "lobby_task.ovl", 0x00200000u);
    mhp3rd::mods::native::frame(runtime);
    runtime.memory().store32(kBuffer + 16u, 0x00FFFFFFu);
    runtime.cpu().gpr[8] = kBuffer + 16u;
    (void)call(runtime, 0x0889EEC4u, 0u);
    check(runtime.memory().load32(kBuffer + 16u) == 0x00FFFFFFu,
        "monsters: once the quest ends, its models are forgotten");
    place_overlay(runtime, kSlot, "game_task.ovl", 0x00200000u);
    traced_code();
    mhp3rd::mods::native::overlay_loaded(runtime, kSlot);

    // Through the service: files from the mod's folder.
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "yakumo_monster_test";
    std::filesystem::create_directories(folder / "model");
    std::ofstream(folder / "model" / "model.pac", std::ios::binary)
        .write(reinterpret_cast<const char *>(mine.data()), static_cast<std::streamsize>(mine.size()));
    std::ofstream(folder / "model" / "ge.bin", std::ios::binary) << "GE";
    std::ofstream(folder / "model" / "motions.bin", std::ios::binary) << "MOTIONS";
    // Its picture: a PNG of 42 by 42 pixels in two colours.
    std::vector<std::uint8_t> picture(42u * 42u * 4u, 0xFFu);
    std::fill(picture.begin(), picture.begin() + 42u * 4u, std::uint8_t{0x20u});
    int png_length = 0;
    unsigned char *png = stbi_write_png_to_mem(picture.data(), 42 * 4, 42, 42, 4, &png_length);
    std::ofstream(folder / "model" / "picture.png", std::ios::binary)
        .write(reinterpret_cast<const char *>(png), png_length);
    STBIW_FREE(png);
    std::ofstream(folder / "model" / "bad.png", std::ios::binary) << "not a picture";
    tables::write_traced_code_for_tests(runtime, "monster.pictures");
    monsters::read_packs_with_for_tests([&base](mhp3rd::mods::FileId) { return std::optional(base); });
    ymod_mod *mod = mhp3rd::mods::native::add_mod("test.monsters", "Monster test", folder.string());
    const auto *service =
        static_cast<const ymod_monsters *>(mhp3rd::mods::native::host()->service(mod, YMOD_MONSTERS, 1u));
    ymod_monster spec{
        sizeof(ymod_monster), "black_fatalis", "tigrex", "model/model.pac", "model/ge.bin", "model/motions.bin"};
    spec.picture = "model/picture.png";
    check(service != nullptr && service->add(mod, &spec) == 76 && service->kind("black_fatalis") == 76 &&
            service->kind("tigrex") == 58 && service->kind("nothing") == -1 && service->kind(nullptr) == -1 &&
            std::string(service->id_of(76)) == "black_fatalis" && service->id_of(255) == nullptr &&
            service->id_of(-1) == nullptr,
        "monsters: the service adds a kind from the mod's files and names kinds");
    const auto picture_row = [&runtime](std::uint32_t kind) {
        const std::uint32_t at = tables::placement("monster.pictures")->address + 6u * kind;
        return std::array<std::uint16_t, 3>{
            runtime.memory().load16(at), runtime.memory().load16(at + 2u), runtime.memory().load16(at + 4u)};
    };
    check(tables::placement("monster.pictures")->rows == 77u && picture_row(76u) != picture_row(58u) &&
            std::none_of(mhp3rd::mods::native::reports().begin(), mhp3rd::mods::native::reports().end(),
                [](const auto &report) { return report.line.starts_with("monster black_fatalis: it"); }),
        "monsters: a kind given a picture gets a cell of its own for it");
    ymod_monster escape = spec;
    escape.id = "escaped";
    escape.model = "../yakumo_monster_test/model/model.pac";
    ymod_monster missing = spec;
    missing.id = "missing";
    missing.motions = "model/none.bin";
    ymod_monster old{sizeof(std::uint32_t)};
    check(service != nullptr && service->add(mod, &escape) == -1 &&
            logged("monster: the model file ../yakumo_monster_test/model/model.pac is not in the mod's folder") &&
            service->add(mod, &missing) == -1 && service->add(mod, &old) == -1 && service->add(mod, nullptr) == -1,
        "monsters: files outside the mod's folder, missing files and short structs are refused");
    ymod_monster unknown = spec;
    unknown.id = "unknown";
    unknown.based_on = "kirin";
    unknown.picture = "model/bad.png";
    check(service != nullptr && service->add(mod, &unknown) == -1 &&
            logged("monster unknown is not added: the base \"kirin\" is not one of the game's kinds"),
        "monsters: the service says why a kind is not added");
    check(reported("Monster test", "monster: the picture file model/bad.png is not a PNG of 42 by 42 pixels"),
        "monsters: a picture that is not a PNG of 42 by 42 pixels is reported");
    // Behaviour: the kind's monsters get a vtable of their own, whose THINK
    // runs the mod's function first.
    struct Seen {
        int calls{};
        std::uint32_t monster{};
        int answer{YMOD_CONTINUE};
    } thought;
    const ymod_monster_fn think = [](ymod_cpu *cpu, std::uint32_t monster, void *user) {
        auto *s = static_cast<Seen *>(user);
        ++s->calls;
        s->monster = monster;
        cpu->gpr[2] = 9u;
        return s->answer;
    };
    check(service != nullptr && service->behave(mod, 58, YMOD_MONSTER_THINK, think, &thought) == -1 &&
            service->behave(mod, 75, YMOD_MONSTER_THINK, think, &thought) == -1 &&
            service->behave(mod, 76, 7, think, &thought) == -1 &&
            service->behave(mod, 76, YMOD_MONSTER_THINK, nullptr, &thought) == -1 &&
            logged("monster 75 is not given that behaviour: the kind was not added by this mod"),
        "monsters: behaviour only for a kind the mod added, a part that exists and a function");
    constexpr std::uint32_t kMonster = 0x08B40000u, kBaseTable = 0x08B41000u, kBaseThink = 0x08B42000u;
    for (std::uint32_t slot = 0; slot < 96u; ++slot)
        runtime.memory().store32(kBaseTable + slot * 4u, 0x08B43000u + slot);
    runtime.memory().store32(kBaseTable + 32u * 4u, kBaseThink);
    store(runtime, kBaseThink, {0x24020007u, 0x03E00008u, 0u}); // v0 = 7
    runtime.memory().store32(kBaseTable + 33u * 4u, kBaseThink + 0x100u);
    store(runtime, kBaseThink + 0x100u, {0x24020005u, 0x03E00008u, 0u}); // Act: v0 = 5
    runtime.memory().store32(kBaseTable + 36u * 4u, kBaseThink + 0x200u);
    store(runtime, kBaseThink + 0x200u, {0x24020006u, 0x03E00008u, 0u}); // Effects: v0 = 6
    runtime.memory().store32(kMonster, kBaseTable);
    runtime.memory().store8(kMonster + 0x62u, 76u);
    // make(manager, kind) -> the monster, as game_task's maker returns it.
    store(runtime, 0x0A090AE0u, {0x3C020000u | (kMonster >> 16u), 0x34420000u | (kMonster & 0xFFFFu), 0x03E00008u, 0u});
    check(service != nullptr && service->behave(mod, 76, YMOD_MONSTER_THINK, think, &thought) == 0,
        "monsters: a kind the mod added takes a part of behaviour");
    mhp3rd::mods::native::overlay_loaded(runtime, kSlot);
    check(call(runtime, 0x0A090AE0u, 0u) == kMonster && runtime.memory().load32(kMonster) != kBaseTable &&
            runtime.memory().load32(runtime.memory().load32(kMonster)) == 0x08B43000u,
        "monsters: a monster of the kind gets its own vtable, a copy of the base's");
    const std::uint32_t own = runtime.memory().load32(kMonster);
    const std::uint32_t stub = runtime.memory().load32(own + 32u * 4u);
    check(
        stub != kBaseThink && call(runtime, stub, kMonster) == 7u && thought.calls == 1 && thought.monster == kMonster,
        "monsters: THINK runs the mod's function, then the base's");
    // Act, which the mod does not give: the base's, unless the mod's Think
    // did the frame's thinking alone.
    const std::uint32_t act_stub = runtime.memory().load32(own + 33u * 4u);
    runtime.cpu().gpr[2] = 0u;
    const std::uint32_t effects_stub = runtime.memory().load32(own + 36u * 4u);
    check(act_stub != kBaseThink + 0x100u && call(runtime, act_stub, kMonster) == 5u &&
            effects_stub != kBaseThink + 0x200u && call(runtime, effects_stub, kMonster) == 6u,
        "monsters: the base's Act and Effects follow the base's Think");
    thought.answer = YMOD_RETURN;
    check(call(runtime, stub, kMonster) == 9u && thought.calls == 2,
        "monsters: and only the mod's when it has done the part");
    runtime.cpu().gpr[2] = 0u;
    check(call(runtime, act_stub, kMonster) == 0u && call(runtime, effects_stub, kMonster) == 0u,
        "monsters: then the base's Act and Effects are left out too");
    runtime.memory().store32(kMonster, kBaseTable);
    check(call(runtime, 0x0A090AE0u, 0u) == kMonster && runtime.memory().load32(kMonster) == own,
        "monsters: every monster of the kind shares the kind's vtable");
    // The base's AI in another monster slot: its own vtable, with its own
    // functions there. A monster made with it gets a copy of that one.
    constexpr std::uint32_t kSecondTable = 0x08B48000u, kSecondThink = 0x08B49000u;
    for (std::uint32_t slot = 0; slot < 96u; ++slot)
        runtime.memory().store32(kSecondTable + slot * 4u, runtime.memory().load32(kBaseTable + slot * 4u));
    runtime.memory().store32(kSecondTable + 32u * 4u, kSecondThink);
    store(runtime, kSecondThink, {0x24020008u, 0x03E00008u, 0u}); // v0 = 8
    runtime.memory().store32(kMonster, kSecondTable);
    (void)call(runtime, 0x0A090AE0u, 0u);
    const std::uint32_t second_own = runtime.memory().load32(kMonster);
    const std::uint32_t second_stub = runtime.memory().load32(second_own + 32u * 4u);
    thought.answer = YMOD_CONTINUE;
    const int calls_now = thought.calls;
    check(second_own != own && second_own != kSecondTable && call(runtime, second_stub, kMonster) == 8u &&
            thought.calls == calls_now + 1 && call(runtime, stub, kMonster) == 7u,
        "monsters: a monster made with the base's vtable of another slot gets a copy of that one, leading to its "
        "functions");
    // Data after the class's 70 slots changing is not other code.
    runtime.memory().store32(kBaseTable + 80u * 4u, 0x12345678u);
    runtime.memory().store32(kMonster, kBaseTable);
    (void)call(runtime, 0x0A090AE0u, 0u);
    check(runtime.memory().load32(kMonster) == own, "monsters: words past the class's slots changing keep the copy");
    runtime.memory().store32(kBaseTable + 80u * 4u, 0x08B43000u + 80u);
    // Other code in the first slot since: the copy is made again from it.
    runtime.memory().store32(kBaseTable + 40u * 4u, 0x08B4A000u);
    runtime.memory().store32(kMonster, kBaseTable);
    (void)call(runtime, 0x0A090AE0u, 0u);
    const std::uint32_t remade = runtime.memory().load32(kMonster);
    check(remade != own && remade != second_own && runtime.memory().load32(remade + 40u * 4u) == 0x08B4A000u &&
            call(runtime, runtime.memory().load32(remade + 32u * 4u), kMonster) == 7u,
        "monsters: a copy whose base vtable has changed since is made again");
    runtime.memory().store32(kBaseTable + 40u * 4u, 0x08B43000u + 40u);
    runtime.memory().store8(kMonster + 0x62u, 58u);
    runtime.memory().store32(kMonster, kBaseTable);
    check(call(runtime, 0x0A090AE0u, 0u) == kMonster && runtime.memory().load32(kMonster) == kBaseTable,
        "monsters: other kinds' monsters keep theirs");
    check(service != nullptr && service->behave(mod, 76, YMOD_MONSTER_THINK, think, &thought) == -1,
        "monsters: behaviour is given before its monsters are made");
    // What a part does: a motion, played by the game's setter once the part's
    // function returns, then the base's part.
    constexpr std::uint32_t kPlayed = 0x08B44000u;
    // play(monster, motion, blend): records its arguments.
    store(runtime, 0x0A0B3004u,
        {0x30A5FFFFu, 0x3C080000u | (kPlayed >> 16u), 0xAD050000u | (kPlayed & 0xFFFFu),
            0xAD060004u | (kPlayed & 0xFFFFu), 0x03E00008u, 0u});
    struct Acting {
        const ymod_monsters *service{};
        int answer{YMOD_CONTINUE};
        const ymod_calls *calls{};
    } acting{service};
    check(service != nullptr && service->play_motion(kMonster, 3, 4) == -1,
        "monsters: a motion is played only from a part of behaviour");
    const ymod_monster_fn act = [](ymod_cpu *cpu, std::uint32_t monster, void *user) -> int {
        auto *a = static_cast<Acting *>(user);
        if (a->service->play_motion(monster, 0x10012, 4) != -1 || a->service->play_motion(monster, 12, -1) != -1)
            return YMOD_RETURN;
        (void)a->service->play_motion(monster, 12, 4);
        if (a->calls != nullptr) {
            const std::uint32_t arguments[4]{};
            (void)a->calls->call(cpu, 0x08B48000u, arguments, nullptr, nullptr);
        }
        return a->answer;
    };
    ymod_monster second = spec;
    second.id = "acting_fatalis";
    const int32_t acting_kind = service != nullptr ? service->add(mod, &second) : -1;
    check(acting_kind == 77 && service->behave(mod, acting_kind, YMOD_MONSTER_THINK, act, &acting) == 0,
        "monsters: a second kind takes a part that acts");
    runtime.memory().store32(kMonster, kBaseTable);
    runtime.memory().store8(kMonster + 0x62u, 77u);
    (void)call(runtime, 0x0A090AE0u, 0u);
    const std::uint32_t acting_stub = runtime.memory().load32(runtime.memory().load32(kMonster) + 32u * 4u);
    runtime.memory().store32(kPlayed, 0u);
    check(call(runtime, acting_stub, kMonster) == 7u && runtime.memory().load32(kPlayed) == 12u &&
            runtime.memory().load32(kPlayed + 4u) == 4u && runtime.cpu().pc == kSentinel,
        "monsters: the motion it asked for is played, then the base's part runs and returns to the game");
    acting.answer = YMOD_RETURN;
    runtime.memory().store32(kPlayed, 0u);
    runtime.cpu().gpr[2] = 0u;
    check(call(runtime, acting_stub, kMonster) != 7u && runtime.memory().load32(kPlayed) == 12u &&
            runtime.cpu().pc == kSentinel,
        "monsters: and without the base's when the part is done");
    // A part that calls guest code itself and asks for a motion too: the call
    // runs and returns to the game, the motion is left out, and its mod's page
    // says why.
    store(runtime, 0x08B48000u, {0x2402002Au, 0x03E00008u, 0u}); // v0 = 42
    acting.calls = static_cast<const ymod_calls *>(mhp3rd::mods::native::host()->service(mod, YMOD_CALLS, 1u));
    runtime.memory().store32(kPlayed, 0u);
    check(acting.calls != nullptr && call(runtime, acting_stub, kMonster) == 42u &&
            runtime.memory().load32(kPlayed) == 0u && runtime.cpu().pc == kSentinel &&
            reported("Monster test",
                "monster acting_fatalis: a part of its behaviour called guest code and queued "
                "motions or attacks in the same call; those are left out"),
        "monsters: a part that calls guest code itself has what it queued left out, and is told");
    acting.calls = nullptr;

    // Whether the motion is over, as the game works it out from the model.
    constexpr std::uint32_t kGetter = 0x08B45000u, kTable2 = 0x08B46000u, kActor = 0x08B47000u;
    store(runtime, kGetter, {0x03E00008u, 0x24820E00u}); // jr ra; addiu v0, a0, 0xE00
    runtime.memory().store32(kTable2 + 17u * 4u, kGetter);
    runtime.memory().store32(kActor, kTable2);
    const auto set = [&runtime](std::uint32_t at, float value) {
        runtime.memory().store32(at, std::bit_cast<std::uint32_t>(value));
    };
    const std::uint32_t actor_model = kActor + 0xE00u;
    set(actor_model + 4u, 10.0f);
    set(actor_model + 12u, 1.0f);
    set(actor_model + 20u, 10.5f);
    check(service != nullptr && service->motion_finished(kActor) == 1,
        "monsters: a motion whose next step passes its last frame is over");
    set(actor_model + 20u, 20.0f);
    check(service != nullptr && service->motion_finished(kActor) == 0, "monsters: one with frames left is not");
    set(actor_model + 20u, 10.5f);
    runtime.memory().store16(actor_model + 80u, 2u);
    check(service != nullptr && service->motion_finished(kActor) == 0, "monsters: a looping one never is");
    runtime.memory().store16(actor_model + 80u, 0u);
    runtime.memory().store32(actor_model + 40u, 1u);
    check(service != nullptr && service->motion_finished(kActor) == 0, "monsters: nor one held still");
    // Frames: the step from 10 to 11 passes 10.5 and 11, not 10 or 12.
    runtime.memory().store32(actor_model + 40u, 0u);
    set(actor_model + 20u, 30.0f);
    float reached = 0.0f;
    check(service != nullptr && service->motion_frame(kActor, &reached) == 0 && reached == 10.0f &&
            service->motion_frame(kActor, nullptr) == -1,
        "monsters: the frame the motion has reached");
    check(service != nullptr && service->at_frame(kActor, 10.5f) == 1 && service->at_frame(kActor, 11.0f) == 1 &&
            service->at_frame(kActor, 10.0f) == 0 && service->at_frame(kActor, 12.0f) == 0,
        "monsters: a frame is passed in the step that reaches it");
    set(actor_model + 20u, 10.5f);
    check(service != nullptr && service->at_frame(kActor, 10.5f) == 1 && service->at_frame(kActor, 10.8f) == 0,
        "monsters: not beyond the motion's last frame");
    runtime.memory().store16(actor_model + 80u, 1u);
    set(actor_model + 16u, 2.0f);
    check(service != nullptr && service->at_frame(kActor, 10.8f) == 1 && service->at_frame(kActor, 2.0f) == 1 &&
            service->at_frame(kActor, 3.0f) == 0,
        "monsters: a motion wrapping round passes its loop's start");
    runtime.memory().store32(actor_model + 40u, 1u);
    check(service != nullptr && service->at_frame(kActor, 10.8f) == 0, "monsters: one held still passes no frame");
    runtime.memory().store16(actor_model + 80u, 0u);
    runtime.memory().store32(actor_model + 40u, 0u);
    runtime.memory().store32(kGetter, 0u);
    check(service != nullptr && service->motion_finished(kActor) == -1 && service->motion_finished(4u) == -1 &&
            service->at_frame(kActor, 1.0f) == -1 && service->motion_frame(kActor, &reached) == -1,
        "monsters: a model that cannot be found gives no answer");

    // The body: where it is, where it looks, where it goes.
    set(kActor + 0x80u, 100.0f);
    set(kActor + 0x84u, 5.0f);
    set(kActor + 0x88u, 200.0f);
    runtime.memory().store32(kActor + 0x74u, 0x4000u); // a quarter turn: towards +x
    set(kActor + 0x4C0u, 100.0f);
    set(kActor + 0x4C4u, 0.0f);
    set(kActor + 0x4C8u, 1200.0f); // straight along +z
    ymod_vec3 at{}, to{};
    float looks = 0.0f;
    check(service != nullptr && service->position(kActor, &at) == 0 && at.x == 100.0f && at.y == 5.0f &&
            at.z == 200.0f && service->target(kActor, &to) == 0 && to.z == 1200.0f &&
            service->facing(kActor, &looks) == 0 && looks == 90.0f,
        "monsters: its position, target and facing in degrees");
    runtime.memory().store16(kActor + 0x246u, 1234u);
    runtime.memory().store16(kActor + 0x288u, 5400u);
    std::int32_t now = 0, most = 0;
    check(service != nullptr && service->health(kActor, &now, &most) == 0 && now == 1234 && most == 5400 &&
            service->health(kActor, nullptr, nullptr) == 0 && service->health(4u, &now, &most) == -1,
        "monsters: its health now and at most");
    check(service != nullptr && service->position(4u, &at) == -1 && service->position(kActor, nullptr) == -1 &&
            service->facing(4u, &looks) == -1 && service->set_facing(kActor, std::nanf("")) == -1,
        "monsters: nothing for an address that is not a monster");
    check(service != nullptr && service->turn_to_target(kActor, 45.0f) == 0 &&
            runtime.memory().load32(kActor + 0x74u) == 0x2000u && service->turn_to_target(kActor, 45.0f) == 1 &&
            runtime.memory().load32(kActor + 0x74u) == 0u && service->turn_to_target(kActor, 45.0f) == 1,
        "monsters: it turns towards its target the shorter way, by at most the step");
    set(kActor + 0x4C0u, 0.0f);
    set(kActor + 0x4C8u, 200.0f); // along -x: the shorter way is through 270
    check(service != nullptr && service->turn_to_target(kActor, 10.0f) == 0 &&
            runtime.memory().load32(kActor + 0x74u) == 0x10000u - 0x071Cu,
        "monsters: and the other way round when that is shorter");
    const auto where = [service] {
        ymod_vec3 out{};
        (void)service->position(kActor, &out);
        return out;
    };
    check(service != nullptr && service->set_facing(kActor, -90.0f) == 0 &&
            runtime.memory().load32(kActor + 0x74u) == 0xC000u && service->facing(kActor, &looks) == 0 &&
            looks == 270.0f,
        "monsters: facings set in degrees");
    check(service != nullptr && service->move(kActor, 0.0f, 50.0f) == 0 && std::abs(where().x - 50.0f) < 0.001f &&
            std::abs(where().z - 200.0f) < 0.001f,
        "monsters: it moves along its facing");
    check(service != nullptr && service->move(kActor, 30.0f, 0.0f) == 0 && std::abs(where().x - 50.0f) < 0.001f &&
            std::abs(where().z - 230.0f) < 0.001f,
        "monsters: and to its side, as its motions' x");
    check(service != nullptr && service->set_facing(kActor, 405.0f) == 0 && service->facing(kActor, &looks) == 0 &&
            looks == 45.0f && service->move(kActor, 0.0f, std::nanf("")) == -1 && service->move(4u, 0.0f, 1.0f) == -1,
        "monsters: facings wrap round, and nothing moves by no number");

    // Attacks, queued from a part as motions are: the game's start and end.
    constexpr std::uint32_t kAttacked = 0x08B50100u; // a low half the stores take unsigned
    store(runtime, 0x0A09E884u,
        {0x27BDFFE0u, 0xAFB60018u, 0x3C080000u | (kAttacked >> 16u), 0xAD050000u | (kAttacked & 0xFFFFu),
            0xAD060004u | (kAttacked & 0xFFFFu), 0xAD070008u | (kAttacked & 0xFFFFu), 0x03E00008u, 0x27BD0020u});
    store(runtime, 0x0A0AFEB0u,
        {0x27BDFF80u, 0xAFB10064u, 0x3C080000u | (kAttacked >> 16u), 0xAD040010u | (kAttacked & 0xFFFFu), 0x03E00008u,
            0x27BD0080u});
    store(runtime, 0x0A09B47Cu,
        {0x30A500FFu, 0x00052880u, 0x3C080000u | (kAttacked >> 16u), 0xAD05000Cu | (kAttacked & 0xFFFFu), 0x03E00008u,
            0u});
    check(service != nullptr && service->attack(kMonster, 1, 6) == -1 && service->end_attack(kMonster, 1) == -1 &&
            service->aim(kMonster) == -1,
        "monsters: attacks only from a part of behaviour");
    const ymod_monster_fn attacking = [](ymod_cpu *, std::uint32_t monster, void *user) -> int {
        const auto *a = static_cast<Acting *>(user);
        // A monster has attack slots 0 and 1, and a kind 64 attack rows at most.
        if (a->service->attack(monster, 256, 6) != -1 || a->service->attack(monster, 1, -1) != -1 ||
            a->service->end_attack(monster, -1) != -1 || a->service->attack(monster, 2, 6) != -1 ||
            a->service->attack(monster, 1, 64) != -1 || a->service->end_attack(monster, 2) != -1)
            return YMOD_CONTINUE;
        (void)a->service->attack(monster, 1, 23);
        (void)a->service->end_attack(monster, 0);
        (void)a->service->aim(monster);
        return YMOD_RETURN;
    };
    ymod_monster third = spec;
    third.id = "attacking_fatalis";
    const int32_t attacking_kind = service != nullptr ? service->add(mod, &third) : -1;
    check(attacking_kind == 78 && service->behave(mod, attacking_kind, YMOD_MONSTER_THINK, attacking, &acting) == 0,
        "monsters: a third kind takes a part that attacks");
    runtime.memory().store32(kMonster, kBaseTable);
    runtime.memory().store8(kMonster + 0x62u, 78u);
    (void)call(runtime, 0x0A090AE0u, 0u);
    const std::uint32_t attacking_stub = runtime.memory().load32(runtime.memory().load32(kMonster) + 32u * 4u);
    runtime.memory().store32(kAttacked + 12u, 0xFFFFFFFFu);
    // Its own attack shape: the kind's data the monster keeps becomes a copy
    // whose shape 2 is the mod's spheres.
    constexpr std::uint32_t kKindData = 0x08B51000u, kTables = 0x08B51100u, kShapeList = 0x08B51200u;
    for (std::uint32_t at = 0; at < 0x300u; at += 4u) runtime.memory().store32(kKindData + at, 0u);
    runtime.memory().store32(kKindData + 8u, 0x3F800000u);
    runtime.memory().store32(kKindData + 0xA4u, kTables);
    runtime.memory().store32(kTables + 0x5Cu, kShapeList);
    runtime.memory().store32(kTables + 0x60u, 0x08B51280u);
    for (std::uint32_t shape = 0; shape < 32u; ++shape)
        runtime.memory().store32(kShapeList + shape * 4u, 0x08B52000u + shape);
    runtime.memory().store32(kMonster + 0xD50u, kKindData);
    const ymod_hit_sphere jaws[] = {{51, 200.0f, 0.0f, 10.0f, 20.0f}, {53, 150.0f, 0.0f, 0.0f, 0.0f}};
    const ymod_hit_sphere broken[] = {{-1, 200.0f, 0.0f, 0.0f, 0.0f}};
    const ymod_hit_sphere flat[] = {{3, 0.0f, 0.0f, 0.0f, 0.0f}};
    check(service != nullptr && service->attack_shape(mod, 58, 2, jaws, 2, sizeof(ymod_hit_sphere)) == -1 &&
            service->attack_shape(mod, 78, 32, jaws, 2, sizeof(ymod_hit_sphere)) == -1 &&
            service->attack_shape(mod, 78, 2, jaws, 0, sizeof(ymod_hit_sphere)) == -1 &&
            service->attack_shape(mod, 78, 2, nullptr, 2, sizeof(ymod_hit_sphere)) == -1 &&
            service->attack_shape(mod, 78, 2, broken, 1, sizeof(ymod_hit_sphere)) == -1 &&
            service->attack_shape(mod, 78, 2, flat, 1, sizeof(ymod_hit_sphere)) == -1 &&
            logged("monster 78 is not given attack shape 2: a sphere needs a joint, a radius above 0 and a centre"),
        "monsters: attack shapes only for the mod's kind, a shape that exists and usable spheres");
    check(service != nullptr && service->attack_shape(mod, 78, 2, jaws, 2, sizeof(ymod_hit_sphere)) == 0,
        "monsters: a kind the mod added takes an attack shape of its own");
    const std::uint32_t attacked_v0 = call(runtime, attacking_stub, kMonster);
    check(attacked_v0 != 7u && runtime.memory().load32(kAttacked) == 1u &&
            runtime.memory().load32(kAttacked + 4u) == 23u && runtime.memory().load32(kAttacked + 8u) == 0u &&
            runtime.memory().load32(kAttacked + 12u) == 0u && runtime.memory().load32(kAttacked + 16u) == kMonster &&
            runtime.cpu().pc == kSentinel,
        "monsters: the attack starts in its slot, the other slot's ends, and the target is brought up to date");
    const std::uint32_t own_data = runtime.memory().load32(kMonster + 0xD50u);
    const std::uint32_t own_tables = runtime.memory().load32(own_data + 0xA4u);
    const std::uint32_t own_list = runtime.memory().load32(own_tables + 0x5Cu);
    const std::uint32_t sphere = runtime.memory().load32(own_list + 2u * 4u);
    const auto real = [&runtime](std::uint32_t at) { return std::bit_cast<float>(runtime.memory().load32(at)); };
    check(own_data != kKindData && runtime.memory().load32(own_data + 8u) == 0x3F800000u && own_tables != kTables &&
            runtime.memory().load32(own_tables + 0x60u) == 0x08B51280u && own_list != kShapeList &&
            runtime.memory().load32(own_list) == 0x08B52000u && runtime.memory().load32(sphere) == 51u &&
            real(sphere + 12u) == 200.0f && real(sphere + 20u) == 10.0f && real(sphere + 24u) == 20.0f &&
            runtime.memory().load32(sphere + 40u) == 53u && runtime.memory().load32(sphere + 80u) == 0xFFFFu &&
            runtime.memory().load32(kShapeList + 8u) == 0x08B52002u,
        "monsters: the monster keeps a copy of the kind's data with the mod's spheres, the base's left as it was");
    (void)call(runtime, attacking_stub, kMonster);
    check(runtime.memory().load32(kMonster + 0xD50u) == own_data, "monsters: and keeps the same copy afterwards");

    // Body capsules: the kind's data a monster keeps points at the mod's, twice.
    const auto shapes_of = [](psprecomp::Runtime &r, std::uint32_t data) {
        return r.memory().load32(r.memory().load32(data + 0xA4u) + 0x5Cu);
    };
    const ymod_body_capsule body[] = {{51, 55, 0, 0x0u, 110.0f, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 20.0f}},
        {0, -1, 0, 0x120000u, 260.0f, {0.0f, 10.0f, 0.0f}, {0.0f, 0.0f, 0.0f}}};
    const ymod_body_capsule thin[] = {{51, 55, 0, 0u, 0.0f, {}, {}}};
    check(service != nullptr && service->body_shapes(mod, 58, body, 2, sizeof(ymod_body_capsule)) == -1 &&
            service->body_shapes(mod, 78, body, 0, sizeof(ymod_body_capsule)) == -1 &&
            service->body_shapes(mod, 78, nullptr, 2, sizeof(ymod_body_capsule)) == -1 &&
            service->body_shapes(mod, 78, thin, 1, sizeof(ymod_body_capsule)) == -1 &&
            logged(
                "monster 78 is not given body capsules: a capsule needs joints, a part, a radius above 0 and its ends"),
        "monsters: body capsules only for the mod's kind, 1 to 64 of them, each usable");
    runtime.memory().store32(kMonster + 0xD50u, kKindData);
    check(service != nullptr && service->body_shapes(mod, 78, body, 2, sizeof(ymod_body_capsule)) == 0,
        "monsters: a kind the mod added takes body capsules of its own");
    (void)call(runtime, attacking_stub, kMonster);
    const std::uint32_t bodied = runtime.memory().load32(kMonster + 0xD50u);
    const std::uint32_t capsules = runtime.memory().load32(bodied + 0x1Cu);
    check(bodied != kKindData && runtime.memory().load32(bodied + 0x20u) == capsules &&
            runtime.memory().load16(capsules) == 51u && runtime.memory().load16(capsules + 2u) == 55u &&
            runtime.memory().load16(capsules + 4u) == 2u && real(capsules + 16u) == 110.0f &&
            real(capsules + 40u) == 20.0f && runtime.memory().load16(capsules + 44u) == 0u &&
            runtime.memory().load16(capsules + 48u) == 0u && runtime.memory().load32(capsules + 52u) == 0x120000u &&
            real(capsules + 68u) == 10.0f && runtime.memory().load16(capsules + 88u) == 0xFFFFu &&
            runtime.memory().load32(capsules + 96u) == 0xFFFFu &&
            runtime.memory().load32(runtime.memory().load32(shapes_of(runtime, bodied) + 8u)) == 51u,
        "monsters: its copy of the kind's data lists the mod's capsules (a sphere where joint_to is -1), ended, with "
        "its attack shapes kept");
    // The same capsules from a mod built with wider ones, as a later SDK's:
    // read by the stride given, their extra bytes ignored, they make the same
    // list. A stride below the first SDK's capsule is refused.
    struct WideCapsule {
        ymod_body_capsule capsule;
        float later[3];
    };
    const WideCapsule wide_body[] = {{body[0], {-1.0f, -1.0f, -1.0f}}, {body[1], {-1.0f, -1.0f, -1.0f}}};
    std::vector<std::uint8_t> listed(100u);
    runtime.memory().copy_out(capsules, listed);
    runtime.memory().store32(kMonster + 0xD50u, kKindData);
    const bool wide_given = service != nullptr &&
        service->body_shapes(mod, 78, &wide_body[0].capsule, 2, static_cast<std::uint32_t>(sizeof(WideCapsule))) == 0;
    (void)call(runtime, attacking_stub, kMonster);
    std::vector<std::uint8_t> wide_listed(100u);
    runtime.memory().copy_out(runtime.memory().load32(runtime.memory().load32(kMonster + 0xD50u) + 0x1Cu), wide_listed);
    check(wide_given && wide_listed == listed,
        "monsters: capsules wider than this header's are read by their stride, their extra bytes ignored");
    constexpr std::size_t kCapsuleEnd = offsetof(ymod_body_capsule, to) + sizeof(ymod_vec3);
    check(service != nullptr && service->body_shapes(mod, 78, body, 2, 8u) == -1 &&
            logged("monster 78 is not given body capsules: the stride is 8, less than the " +
                std::to_string(kCapsuleEnd) + " bytes of a ymod_body_capsule: set it to sizeof(ymod_body_capsule)") &&
            service->attack_shape(mod, 78, 2, jaws, 2, 0u) == -1 &&
            logged("monster 78 is not given attack shape 2: the stride is 0, less than the " +
                std::to_string(offsetof(ymod_hit_sphere, z) + sizeof(float)) +
                " bytes of a ymod_hit_sphere: set it to sizeof(ymod_hit_sphere)"),
        "monsters: capsules or spheres whose stride is below the first SDK's, or 0, are refused with the reason");
    // Attack rows: a row of the kind's own, a base row with another shape.
    constexpr std::uint32_t kRows = 0x08B51280u;
    runtime.memory().store32(kRows + 7u * 28u, 0x00500002u);
    runtime.memory().store8(kRows + 7u * 28u + 0x11u, 3u);
    check(service != nullptr && service->attack_row(mod, 58, 30, 7, 20) == -1 &&
            service->attack_row(mod, 78, 0, 7, 20) == -1 && service->attack_row(mod, 78, 64, 7, 20) == -1 &&
            service->attack_row(mod, 78, 30, 64, 20) == -1 && service->attack_row(mod, 78, 30, -1, 20) == -1 &&
            logged("monster 78 is not given attack row 30: no such attack row") &&
            service->attack_row(mod, 78, 30, 7, 32) == -1 && service->attack_row(mod, 78, 30, 7, -1) == -1 &&
            logged("monster 78 is not given attack row 30: no such attack shape"),
        "monsters: attack rows only for the mod's kind, rows 1 to 63 like a row up to 63, shapes up to 31");
    runtime.memory().store32(kMonster + 0xD50u, kKindData);
    check(service != nullptr && service->attack_row(mod, 78, 30, 7, 20) == 0,
        "monsters: a kind the mod added takes an attack row of its own");
    (void)call(runtime, attacking_stub, kMonster);
    const std::uint32_t rowed = runtime.memory().load32(kMonster + 0xD50u);
    const std::uint32_t own_rows = runtime.memory().load32(runtime.memory().load32(rowed + 0xA4u) + 0x60u);
    check(rowed != kKindData && own_rows != kRows && runtime.memory().load32(own_rows + 30u * 28u) == 0x00500002u &&
            runtime.memory().load8(own_rows + 30u * 28u + 0x11u) == 20u &&
            runtime.memory().load32(own_rows + 7u * 28u) == 0x00500002u &&
            runtime.memory().load8(own_rows + 7u * 28u + 0x11u) == 3u &&
            runtime.memory().load8(kRows + 30u * 28u + 0x11u) != 20u &&
            runtime.memory().load16(runtime.memory().load32(rowed + 0x1Cu)) == 51u,
        "monsters: its copy of the kind's data has the base's rows and the mod's row, a copy of the base row with "
        "its shape, with its capsules kept");
    // Footsteps: after a Think done alone, the game's motion event player gets
    // the kind's tables. A stand-in records its tables, the monster and the
    // flags on the stack.
    store(runtime, 0x0A01476Cu,
        {0x27BDFFD0u, 0x93A30030u, 0x3C180000u | (kAttacked >> 16u), 0xAF070000u | ((kAttacked & 0xFFFFu) + 0x20u),
            0xAF090000u | ((kAttacked & 0xFFFFu) + 0x24u), 0xAF0A0000u | ((kAttacked & 0xFFFFu) + 0x28u),
            0xAF0B0000u | ((kAttacked & 0xFFFFu) + 0x2Cu), 0xAF030000u | ((kAttacked & 0xFFFFu) + 0x30u), 0x03E00008u,
            0x27BD0030u});
    const ymod_footstep steps[] = {{7, 30.0f, 25, 4}, {2, 46.0f, 10, 1}, {7, 60.0f, 30, 4}};
    const ymod_footstep huge[] = {{7, 30.0f, 25, 10}};
    const ymod_footstep late[] = {{1000, 30.0f, 25, 1}};
    check(service != nullptr && service->footsteps(mod, 58, steps, 3, sizeof(ymod_footstep)) == -1 &&
            service->footsteps(mod, 78, steps, 0, sizeof(ymod_footstep)) == -1 &&
            service->footsteps(mod, 78, nullptr, 3, sizeof(ymod_footstep)) == -1 &&
            service->footsteps(mod, 78, huge, 1, sizeof(ymod_footstep)) == -1 &&
            service->footsteps(mod, 78, late, 1, sizeof(ymod_footstep)) == -1 &&
            logged("monster 78 is not given footsteps: a footstep needs a motion below 1000, a frame, a joint and a "
                   "size from 0 to 9"),
        "monsters: footsteps only for the mod's kind, 1 to 256 of them, each usable");
    // A motion's row counts its footsteps in a byte: 255 at most in one.
    std::vector<ymod_footstep> crowded(256u, ymod_footstep{3, 10.0f, 25, 1});
    const bool full_refused = service != nullptr &&
        service->footsteps(mod, 78, crowded.data(), 256, sizeof(ymod_footstep)) == -1 &&
        logged("monster 78 is not given footsteps: motion 3 has more than 255 footsteps");
    crowded.back().motion = 4;
    check(full_refused && service != nullptr &&
            service->footsteps(mod, 78, crowded.data(), 256, sizeof(ymod_footstep)) == 0,
        "monsters: 256 footsteps in one motion are refused, 255 in one and one in another taken");
    check(service != nullptr && service->footsteps(mod, 78, steps, 3, 0x60000000u) == -1 &&
            logged("monster 78 is not given footsteps: the stride is 1610612736: 3 elements that far apart pass 4 GiB"),
        "monsters: footsteps so far apart that they pass 4 GiB are refused before any is read");
    runtime.memory().store32(kAttacked + 0x2Cu, 0u);
    check(service != nullptr && service->footsteps(mod, 78, steps, 3, sizeof(ymod_footstep)) == 0,
        "monsters: a kind the mod added takes footsteps of its own");
    // yakumo/mod.hpp gives the arrays' counts and strides itself, and gives
    // nothing on a host without the function or for a count past an int32_t.
    const yakumo::Mod wrapped(mhp3rd::mods::native::host(), mod);
    check(wrapped.footsteps(78, steps) == 0 && wrapped.body_shapes(78, body) == 0 &&
            wrapped.attack_shape(78, 2, jaws, std::size(jaws)) == 0,
        "sdk: yakumo::Mod passes an array's count and stride to the monsters service");
    static ymod_monsters s_older{};
    s_older = *service;
    s_older.size = static_cast<std::uint32_t>(offsetof(ymod_monsters, attack_shape));
    static const ymod_host s_older_host{YAKUMO_API_VERSION, sizeof(ymod_host),
        [](ymod_mod *, const char *name, std::uint32_t) -> const void * {
            return std::string(name) == YMOD_MONSTERS ? &s_older : nullptr;
        },
        nullptr, nullptr};
    const yakumo::Mod on_older(&s_older_host, mod);
    check(on_older.attack_shape(78, 2, jaws) == -1 && on_older.body_shapes(78, body) == -1 &&
            on_older.footsteps(78, steps) == -1 && wrapped.footsteps(78, steps, std::size_t{1} << 40u) == -1,
        "sdk: yakumo::Mod gives nothing on a host whose monsters service lacks the function, or for a count past "
        "an int32_t");
    (void)call(runtime, attacking_stub, kMonster);
    const std::uint32_t step_list = runtime.memory().load32(kAttacked + 0x20u);
    const std::uint32_t step_rows = runtime.memory().load32(kAttacked + 0x24u);
    const std::uint32_t step_index = runtime.memory().load32(kAttacked + 0x28u);
    check(runtime.memory().load32(kAttacked + 0x2Cu) == kMonster && runtime.memory().load32(kAttacked + 0x30u) == 0u &&
            runtime.memory().load16(step_index + 2u * 2u) == 0u &&
            runtime.memory().load16(step_index + 2u * 7u) == 1u &&
            runtime.memory().load16(step_index + 2u * 3u) == 0xFFFFu && runtime.memory().load16(step_rows + 6u) == 0u &&
            runtime.memory().load8(step_rows + 13u) == 1u && runtime.memory().load16(step_rows + 16u + 6u) == 1u &&
            runtime.memory().load8(step_rows + 16u + 13u) == 2u && runtime.memory().load32(step_list) == 2u &&
            real(step_list + 4u) == 46.0f && runtime.memory().load16(step_list + 8u) == 10u &&
            runtime.memory().load16(step_list + 10u) == 1u && runtime.memory().load32(step_list + 12u) == 7u &&
            real(step_list + 28u) == 60.0f && runtime.memory().load16(step_list + 32u) == 30u,
        "monsters: after its Think, the motion event player gets the kind's footsteps by motion: the index, a row "
        "per motion (where its steps start, how many), each step's motion, frame, joint and size");
    runtime.memory().store32(0x0A01476Cu, 0u);
    runtime.memory().store32(kAttacked + 0x2Cu, 0u);
    (void)call(runtime, attacking_stub, kMonster);
    check(runtime.memory().load32(kAttacked + 0x2Cu) == 0u,
        "monsters: and nothing is called where the game's player is not");
    runtime.memory().store32(0x0A09E884u, 0u);
    const ymod_monster_fn changed = [](ymod_cpu *, std::uint32_t monster, void *user) -> int {
        const auto *a = static_cast<Acting *>(user);
        return a->service->attack(monster, 2, 23) == -1 ? YMOD_RETURN : YMOD_CONTINUE;
    };
    ymod_monster fourth = spec;
    fourth.id = "changed_fatalis";
    const int32_t changed_kind = service != nullptr ? service->add(mod, &fourth) : -1;
    check(changed_kind == 79 && service->behave(mod, changed_kind, YMOD_MONSTER_THINK, changed, &acting) == 0,
        "monsters: a fourth kind takes a part");
    runtime.memory().store32(kMonster, kBaseTable);
    runtime.memory().store8(kMonster + 0x62u, 79u);
    (void)call(runtime, 0x0A090AE0u, 0u);
    const std::uint32_t changed_stub = runtime.memory().load32(runtime.memory().load32(kMonster) + 32u * 4u);
    runtime.cpu().gpr[2] = 0u;
    check(call(runtime, changed_stub, kMonster) != 7u, "monsters: no attack where the game's code is not as traced");

    // Effects: by number, on a joint, through the game's effect starter, with
    // the offset and scale in a frame of their own below the caller's.
    constexpr std::uint32_t kRecord = 0x08B50400u, kSets = 0x08B50800u;
    // Records its arguments; the flags are the first stack word, 64 bytes up
    // once its frame is made.
    store(runtime, 0x0A00D7D8u,
        {0x27BDFFC0u, 0x87A20044u, 0x3C0C0000u | (kRecord >> 16u), 0xAD840000u | (kRecord & 0xFFFFu),
            0xAD850004u | (kRecord & 0xFFFFu), 0xAD860008u | (kRecord & 0xFFFFu), 0xAD87000Cu | (kRecord & 0xFFFFu),
            0x87A20040u, 0xAD820010u | (kRecord & 0xFFFFu), 0x8D0D0008u, 0xAD8D0014u | (kRecord & 0xFFFFu), 0x8D2E0000u,
            0xAD8E0018u | (kRecord & 0xFFFFu), 0x03E00008u, 0x27BD0040u});
    monsters::read_packs_with_for_tests([&fiery](mhp3rd::mods::FileId) { return std::optional(fiery); });
    struct Burning {
        const ymod_monsters *service{};
        int refusals{};
        int started{-2};
    } burning{service};
    const ymod_monster_fn burn = [](ymod_cpu *, std::uint32_t monster, void *user) -> int {
        auto *b = static_cast<Burning *>(user);
        const ymod_vec3 ahead{0.0f, 0.0f, 90.0f};
        b->refusals = (b->service->effect(monster, 0, 51, &ahead, 1.0f) == -1) +
            (b->service->effect(monster, 2, 51, &ahead, 1.0f) == -1) +
            (b->service->effect(monster, 1, 300, &ahead, 1.0f) == -1) +
            (b->service->effect(monster, 1, 51, &ahead, 0.0f) == -1);
        b->started = b->service->effect(monster, 1, 51, &ahead, 0.7f);
        return YMOD_RETURN;
    };
    ymod_monster fifth = spec;
    fifth.id = "burning_fatalis";
    const int32_t burning_kind = service != nullptr ? service->add(mod, &fifth) : -1;
    check(burning_kind == 80 && service->behave(mod, burning_kind, YMOD_MONSTER_THINK, burn, &burning) == 0,
        "monsters: a fifth kind, with effects, takes a part that starts one");
    const std::optional<tables::Placement> model_table = tables::placement("monster.models");
    const std::uint16_t burning_file =
        model_table ? runtime.memory().load16(model_table->address + 2u * (80u - 1u)) : std::uint16_t{0};
    runtime.memory().store32(0x08A38644u, kSets);
    runtime.memory().store16(kSets + 0x90u, 0u); // slot 0: another model set
    runtime.memory().store16(kSets + 0x9Cu, 2u); // slot 1: the kind's
    runtime.memory().store16(kSets + 0x9Eu, burning_file);
    runtime.memory().store32(kMonster, kBaseTable);
    runtime.memory().store8(kMonster + 0x62u, 80u);
    (void)call(runtime, 0x0A090AE0u, 0u);
    const std::uint32_t burning_stub = runtime.memory().load32(runtime.memory().load32(kMonster) + 32u * 4u);
    const std::uint32_t stack = runtime.cpu().gpr[29];
    (void)call(runtime, burning_stub, kMonster);
    check(burning.refusals == 4 && burning.started == 0 && runtime.memory().load32(kRecord) == 13u &&
            runtime.memory().load32(kRecord + 4u) == 1u && runtime.memory().load32(kRecord + 8u) == kMonster &&
            runtime.memory().load32(kRecord + 12u) == 51u && runtime.memory().load32(kRecord + 16u) == 2u &&
            real(kRecord + 20u) == 90.0f && real(kRecord + 24u) == 0.7f && runtime.cpu().gpr[29] == stack &&
            runtime.cpu().pc == kSentinel,
        "monsters: an effect starts in the bank of the kind's loaded model set, with its offset and scale, and the "
        "stack is given back");
    runtime.memory().store16(kSets + 0x9Cu, 0u);
    (void)call(runtime, burning_stub, kMonster);
    check(burning.started == -1, "monsters: no effect while the kind's model set is not loaded");

    // A mod that fails, or is withdrawn, is not called through its kinds
    // again: their monsters do the base's part.
    ymod_mod *quitter = mhp3rd::mods::native::add_mod("test.quitter", "Quitter", folder.string());
    ymod_monster sixth = spec;
    sixth.id = "quitting_fatalis";
    Seen quit_seen;
    const int32_t quitting_kind = service != nullptr ? service->add(quitter, &sixth) : -1;
    check(quitting_kind == 81 && service->behave(quitter, quitting_kind, YMOD_MONSTER_THINK, think, &quit_seen) == 0,
        "monsters: a sixth kind, of another mod, takes a part");
    runtime.memory().store32(kMonster, kBaseTable);
    runtime.memory().store8(kMonster + 0x62u, 81u);
    (void)call(runtime, 0x0A090AE0u, 0u);
    const std::uint32_t quitting_stub = runtime.memory().load32(runtime.memory().load32(kMonster) + 32u * 4u);
    (void)call(runtime, quitting_stub, kMonster);
    const int calls_before = quit_seen.calls;
    mhp3rd::mods::native::withdraw(quitter);
    check(calls_before == 1 && call(runtime, quitting_stub, kMonster) == 7u && quit_seen.calls == 1 &&
            runtime.cpu().pc == kSentinel,
        "monsters: a withdrawn mod's part is not called; the base's runs");

    // A quest to hunt the kind, described through the quests service.
    const auto *quest_service =
        static_cast<const ymod_quests *>(mhp3rd::mods::native::host()->service(mod, YMOD_QUESTS, 1u));
    const ymod_quest_monster hunted{"black_fatalis", 1, {1086.0f, 1.0f, 255.0f}, 0x1E, 0x13, 0};
    ymod_quest quest{sizeof(ymod_quest), 62010u, 8, 8, 50u, 0u, 9000u, "black_fatalis", 1u, &hunted, 1u,
        {"Black Fatalis", "Hunt the Black Fatalis", "Time Over", "In the arena", "Black Fatalis", "Guild"}};
    const mhp3rd::mods::quests::Quest *served = nullptr;
    check(quest_service != nullptr && quest_service->add(mod, &quest) == 62010 &&
            (served = mhp3rd::mods::quests::find(62010u)) != nullptr && served->name == "Black Fatalis" &&
            served->stars == 8u,
        "quests: a mod's quest for a kind it added is built and served");
    ymod_quest unknown_target = quest;
    unknown_target.id = 62011u;
    unknown_target.target = "nothing";
    ymod_quest no_monsters = quest;
    no_monsters.id = 62012u;
    no_monsters.monster_count = 0u;
    ymod_quest out_of_range = quest;
    out_of_range.id = 62013u;
    out_of_range.map = 300;
    ymod_quest old_quest{sizeof(std::uint32_t)};
    check(quest_service != nullptr && quest_service->add(mod, &unknown_target) == -1 &&
            logged("quest 62011 is not added: its target \"nothing\" is not a monster's id") &&
            quest_service->add(mod, &no_monsters) == -1 && quest_service->add(mod, &out_of_range) == -1 &&
            quest_service->add(mod, &quest) == -1 && quest_service->add(mod, &old_quest) == -1 &&
            quest_service->add(mod, nullptr) == -1,
        "quests: an unknown monster, no monsters, a number out of range, a served id and short structs are refused");

    // Rewards named as the game names its items: the quest is built again
    // with them once lobby_task loads, and its file replaced.
    const ymod_quest_reward prizes[] = {{"Wyvern Gem", 1u, 100u, 0u}};
    const ymod_quest_reward odd[] = {{"Wyvern Gem", 1u, 60u, 0u}};
    const ymod_quest_reward unknown_item[] = {{"Unknown Thing", 1u, 100u, 0u}};
    ymod_quest rich = quest;
    rich.id = 62014u;
    rich.rewards = prizes;
    rich.reward_count = 1u;
    ymod_quest uneven = rich;
    uneven.id = 62015u;
    uneven.rewards = odd;
    ymod_quest absent = rich;
    absent.id = 62016u;
    absent.rewards = unknown_item;
    const ymod_quest_reward fourth_group[] = {{"Wyvern Gem", 1u, 100u, 3u}};
    ymod_quest too_many_groups = rich;
    too_many_groups.id = 62017u;
    too_many_groups.rewards = fourth_group;
    // Five: what they add passes the 16 bytes the payload is padded to.
    const ymod_quest_supply box[] = {
        {"Wyvern Gem", 2u}, {"Wyvern Gem", 1u}, {"Wyvern Gem", 3u}, {"Wyvern Gem", 4u}, {"Wyvern Gem", 5u}};
    const ymod_quest_supply empty_box[] = {{"Wyvern Gem", 0u}};
    ymod_quest supplied = quest;
    supplied.id = 62018u;
    supplied.supplies = box;
    supplied.supply_count = 5u;
    ymod_quest bad_supply = supplied;
    bad_supply.id = 62019u;
    bad_supply.supplies = empty_box;
    ymod_quest crowded_box = supplied;
    crowded_box.id = 62020u;
    crowded_box.supply_count = 33u;
    check(quest_service != nullptr && quest_service->add(mod, &uneven) == -1 &&
            logged("quest 62015 is not added: a reward group's chances do not add up to 100, or a group between has "
                   "none") &&
            quest_service->add(mod, &too_many_groups) == -1 &&
            logged("quest 62017 is not added: a reward's item, count, chance or group is out of range") &&
            quest_service->add(mod, &bad_supply) == -1 &&
            logged("quest 62019 is not added: a supply's item or count is out of range") &&
            quest_service->add(mod, &crowded_box) == -1 &&
            logged("quest 62020 is not added: it has more supplies than the box holds") &&
            quest_service->add(mod, &rich) == 62014 && quest_service->add(mod, &absent) == 62016 &&
            quest_service->add(mod, &supplied) == 62018,
        "quests: rewards (in up to three groups) and supplies are checked, then kept for when the game's text is "
        "loaded");
    // Rewards and supplies by their strides: wider ones, as a later SDK's,
    // read with their extra bytes ignored (compared once built with the
    // game's text below); below the first SDK's, refused; and no more rewards
    // than the game's three groups hold.
    struct WideReward {
        ymod_quest_reward reward;
        std::uint32_t later[4];
    };
    struct WideSupply {
        ymod_quest_supply supply;
        const char *later;
    };
    const ymod_quest_reward halves[] = {{"Wyvern Gem", 1u, 50u, 0u}, {"Wyvern Gem", 2u, 50u, 0u}};
    const WideReward wide_halves[] = {{halves[0], {9u, 9u, 9u, 9u}}, {halves[1], {9u, 9u, 9u, 9u}}};
    WideSupply wide_box[5] = {};
    for (std::size_t i = 0; i < 5u; ++i) wide_box[i] = {box[i], "not an item"};
    ymod_quest halved = rich;
    halved.id = 62021u;
    halved.rewards = halves;
    halved.reward_count = 2u;
    ymod_quest wide_halved = halved;
    wide_halved.id = 62022u;
    wide_halved.rewards = &wide_halves[0].reward;
    wide_halved.reward_stride = sizeof(WideReward);
    ymod_quest wide_supplied = supplied;
    wide_supplied.id = 62023u;
    wide_supplied.supplies = &wide_box[0].supply;
    wide_supplied.supply_stride = sizeof(WideSupply);
    ymod_quest narrow_rewards = halved;
    narrow_rewards.id = 62024u;
    narrow_rewards.reward_stride = static_cast<std::uint32_t>(offsetof(ymod_quest_reward, line));
    ymod_quest unstrided_supplies = supplied;
    unstrided_supplies.id = 62025u;
    unstrided_supplies.supply_stride = 0u;
    ymod_quest_reward dirty_halves[] = {halves[0], halves[1]};
    dirty_halves[0].reserved = 0xABABABABu;
    dirty_halves[1].reserved = 0xFFFFFFFFu;
    ymod_quest dirty = halved;
    dirty.id = 62027u;
    dirty.rewards = dirty_halves;
    std::vector<ymod_quest_reward> lavish(73u, ymod_quest_reward{"Wyvern Gem", 1u, 1u, 0u});
    ymod_quest too_many_rewards = rich;
    too_many_rewards.id = 62026u;
    too_many_rewards.rewards = lavish.data();
    too_many_rewards.reward_count = 73u;
    check(quest_service != nullptr && quest_service->add(mod, &halved) == 62021 &&
            quest_service->add(mod, &wide_halved) == 62022 && quest_service->add(mod, &wide_supplied) == 62023 &&
            quest_service->add(mod, &dirty) == 62027,
        "quests: rewards and supplies wider than this header's are taken, and rewards whose reserved field is not 0");
    check(quest_service != nullptr && quest_service->add(mod, &narrow_rewards) == -1 &&
            logged("quest 62024 is not added: reward_stride is " + std::to_string(offsetof(ymod_quest_reward, line)) +
                ", less than the " + std::to_string(sizeof(ymod_quest_reward)) +
                " bytes of a ymod_quest_reward: set it to sizeof(ymod_quest_reward)") &&
            quest_service->add(mod, &unstrided_supplies) == -1 &&
            logged("quest 62025 is not added: supply_stride is 0, less than the " +
                std::to_string(sizeof(ymod_quest_supply)) +
                " bytes of a ymod_quest_supply: set it to sizeof(ymod_quest_supply)") &&
            quest_service->add(mod, &too_many_rewards) == -1 &&
            logged("quest 62026 is not added: it has more than 72 rewards"),
        "quests: a reward or supply stride below the first SDK's, or 0, and more rewards than the game's groups "
        "hold are refused, with the reason");
    const mhp3rd::mods::FileId rich_file = *mhp3rd::mods::p3rd::quest_file(62014u);
    const auto served_size = [&runtime](mhp3rd::mods::FileId file) {
        runtime.cpu().gpr[5] = file;
        return call(runtime, 0x08864E8Cu, 0x08B3F040u);
    };
    const std::uint32_t first_size = served_size(rich_file);
    const mhp3rd::mods::FileId supplied_file = *mhp3rd::mods::p3rd::quest_file(62018u);
    const std::uint32_t first_supplied = served_size(supplied_file);
    // The text block's item names (table 3): 0 a placeholder, 1 the gem.
    constexpr std::uint32_t kBlock = 0x08A40640u, kNames = kBlock + 0x3000u;
    runtime.memory().store32(kBlock + 3u * 4u, 0x3000u);
    runtime.memory().store32(kNames, 12u);
    runtime.memory().store32(kNames + 4u, 14u);
    runtime.memory().store32(kNames + 8u, 0xFFFFFFFFu);
    for (const auto &[at, text] : {std::pair<std::uint32_t, const char *>{12u, "-"}, {14u, "Wyvern Gem"}}) {
        std::uint32_t i = 0;
        for (; text[i] != '\0'; ++i) runtime.memory().store8(kNames + at + i, static_cast<std::uint8_t>(text[i]));
        runtime.memory().store8(kNames + at + i, 0u);
    }
    place_overlay(runtime, 0x0A05E600u, "lobby_task.ovl", 0x00200000u);
    mhp3rd::mods::quests::overlay_loaded(runtime);
    const std::uint32_t second_size = served_size(rich_file);
    check(second_size > first_size && second_size == ((second_size + 15u) & ~15u),
        "quests: once lobby_task loads, a quest with rewards is built again with them and its file replaced");
    check(served_size(supplied_file) > first_supplied, "quests: and one with supplies, with them");
    check(served_size(*mhp3rd::mods::p3rd::quest_file(62016u)) == served_size(*mhp3rd::mods::p3rd::quest_file(62010u)),
        "quests: a reward naming no item of the game's leaves the quest as first built");
    using Payload = std::vector<std::uint8_t>;
    const auto decrypted = [&](std::uint32_t id) -> std::optional<Payload> {
        const mhp3rd::mods::FileId file = *mhp3rd::mods::p3rd::quest_file(id);
        const std::uint32_t size = served_size(file);
        runtime.cpu().gpr[5] = file;
        runtime.cpu().gpr[6] = 0x08C00000u;
        (void)call(runtime, 0x08863CDCu, 0x08B3F040u);
        Payload encrypted(size);
        runtime.memory().copy_out(0x08C00000u, encrypted);
        mhp3rd::game::GuestRam keys_ram(runtime.memory());
        const auto keys = mhp3rd::game::read_quest_keys(keys_ram);
        return keys ? mhp3rd::game::decrypt_quest(encrypted, *keys) : std::nullopt;
    };
    // Built from the same rewards and supplies, the quests differ only in
    // their ids.
    const auto same_but_id = [&](std::uint32_t a, std::uint32_t b) {
        const std::optional<Payload> one = decrypted(a), other = decrypted(b);
        if (!one || !other || one->size() != other->size() || one->size() < 0x100u) return false;
        std::size_t differing = 0u;
        for (std::size_t i = 0; i < one->size(); ++i) differing += (*one)[i] != (*other)[i] ? 1u : 0u;
        return differing > 0u && differing <= 2u;
    };
    check(served_size(*mhp3rd::mods::p3rd::quest_file(62021u)) > first_size && same_but_id(62021u, 62022u) &&
            same_but_id(62018u, 62023u) && same_but_id(62021u, 62027u),
        "quests: wider rewards and supplies are read by their strides, their extra bytes ignored, and a reserved "
        "field is never read: the same quests");

    // What goes wrong with a kind is on its mod's page: with the JIT off, the
    // base's kind checks cannot be hooked.
    ymod_monster unhooked = spec;
    unhooked.id = "unhooked_fatalis";
    psprecomp::set_jit_enabled(false);
    const int32_t unhooked_kind = service != nullptr ? service->add(mod, &unhooked) : -1;
    psprecomp::set_jit_enabled(true);
    check(unhooked_kind > 0 &&
            reported("Monster test",
                "monster unhooked_fatalis: the base's kind check at 0x0A1BB304 (em058m0) cannot be hooked") &&
            reported("Monster test",
                "monster unhooked_fatalis: the base's kind check at 0x0A1F0184 (em058m1) cannot be hooked"),
        "monsters: a kind's problems are on its mod's page");
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    monsters::reset_for_tests();
    added::reset_for_tests();
    tables::reset_for_tests();
}

// A kind with behaviour made from a base that hides no materials: which of its
// monsters thought alone is forgotten when the quest ends, as for any base.
void test_monster_quest_end(psprecomp::Runtime &runtime) {
    namespace monsters = mhp3rd::mods::monsters;
    namespace native = mhp3rd::mods::native;
    namespace tables = mhp3rd::mods::tables;
    tables::reset_for_tests();
    monsters::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
    constexpr std::uint32_t kSlot = 0x0A05E600u;
    place_overlay(runtime, kSlot, "lobby_task.ovl", 0x00200000u);
    native::overlay_loaded(runtime, kSlot);
    for (const char *name : {"monster.models", "monster.models.second", "monster.kinds", "monster.files",
             "monster.constructors", "monster.sizes", "monster.carves"})
        tables::write_traced_code_for_tests(runtime, name);
    const std::vector<std::uint8_t> pack = pack_of({{'S'}, small_pmo(2u), {'T'}});
    monsters::read_packs_with_for_tests([&pack](mhp3rd::mods::FileId) { return std::optional(pack); });
    ymod_mod *mod = native::add_mod("test.quest_end", "Quest End", ".");
    const auto *service = static_cast<const ymod_monsters *>(native::host()->service(mod, YMOD_MONSTERS, 1u));

    // The third model table's code is not as traced: the kind is refused
    // before any table grows or any file is served.
    std::string why;
    check(!monsters::add(runtime, {"refused_rathian", "rathian", pack, {'G'}, {'M'}, "test", mod}, &why) &&
            why == "table monster.models.third cannot grow: the game's code that reads it is not as traced" &&
            tables::placement("monster.kinds")->rows == 75u && tables::placement("monster.models")->rows == 74u &&
            !mhp3rd::mods::added::serves(mhp3rd::mods::added::kLastFreeId) && !monsters::kind("refused_rathian"),
        "monsters: a kind refused for a table that cannot grow leaves every table and file as it was");
    tables::write_traced_code_for_tests(runtime, "monster.models.third");
    // Ids for two of its three files only: those two are taken back.
    namespace added = mhp3rd::mods::added;
    for (std::uint32_t id = added::kFirstFreeId; id < added::kLastFreeId - 1u; ++id)
        (void)added::add(runtime, static_cast<mhp3rd::mods::FileId>(id), {1u});
    check(!monsters::add(runtime, {"crowded_rathian", "rathian", pack, {'G'}, {'M'}, "test", mod}, &why) &&
            why == "its files cannot be served" && !added::serves(added::kLastFreeId) &&
            !added::serves(added::kLastFreeId - 1u) && tables::placement("monster.kinds")->rows == 75u,
        "monsters: a kind whose files cannot all be served takes back those that were, and grows no table");
    added::reset_for_tests();
    std::string reason;
    check(!tables::can_extend(runtime, "no.such.table", 1u, &reason) && reason == "no such table" &&
            !tables::can_extend(runtime, "monster.kinds", 0x10000u) &&
            tables::can_extend(runtime, "monster.kinds", 10u) && !added::remove(added::kLastFreeId),
        "tables: whether a table can grow, for an unknown table, too many rows and rows it has; and no file to take "
        "back");
    const std::optional<std::uint8_t> kind =
        monsters::add(runtime, {"lone_rathian", "rathian", pack, {'G'}, {'M'}, "test", mod}, nullptr);
    const ymod_monster_fn alone = [](ymod_cpu *, std::uint32_t, void *) -> int { return YMOD_RETURN; };
    check(kind && service->behave(mod, *kind, YMOD_MONSTER_THINK, alone, nullptr) == 0,
        "monsters: a kind made from a base without traced behaviour takes a part");
    check(reported("Quest End",
              "monster lone_rathian: its base rathian has no traced behaviour (only tigrex and brute_tigrex do): "
              "where the base's AI checks its own kind, hides materials or writes meshes by number, the kind is not "
              "given the base's way, and it may go another kind's way or stop the game"),
        "monsters: a base without traced behaviour is warned of on the mod's page");
    constexpr std::uint32_t kMonster = 0x08B60000u, kTable = 0x08B61000u, kAct = 0x08B62000u;
    for (std::uint32_t slot = 0; slot < 96u; ++slot) runtime.memory().store32(kTable + slot * 4u, kAct);
    store(runtime, kAct, {0x24020005u, 0x03E00008u, 0u}); // v0 = 5
    runtime.memory().store32(kMonster, kTable);
    runtime.memory().store8(kMonster + 0x62u, *kind);
    store(runtime, 0x0A090AE0u, {0x3C020000u | (kMonster >> 16u), 0x34420000u | (kMonster & 0xFFFFu), 0x03E00008u, 0u});
    place_overlay(runtime, kSlot, "game_task.ovl", 0x00200000u);
    runtime.memory().store32(0x08AB3640u, 0x09000000u); // the character, on quest 501
    runtime.memory().store16(0x09000000u + 0x60472u, 501u);
    native::overlay_loaded(runtime, kSlot);
    (void)call(runtime, 0x0A090AE0u, 0u);
    const std::uint32_t own = runtime.memory().load32(kMonster);
    (void)call(runtime, runtime.memory().load32(own + 32u * 4u), kMonster);
    runtime.cpu().gpr[2] = 0u;
    const std::uint32_t act = runtime.memory().load32(own + 33u * 4u);
    const std::uint32_t during = call(runtime, act, kMonster);
    runtime.memory().store16(0x09000000u + 0x60472u, 1u); // back in the village
    place_overlay(runtime, kSlot, "lobby_task.ovl", 0x00200000u);
    native::overlay_loaded(runtime, kSlot);
    check(during == 0u && call(runtime, act, kMonster) == 5u,
        "monsters: once the quest ends, the base's Act runs again for a monster that thought alone in it");
    // On the next quest one thinks alone again, and a monster made where it
    // was runs the base's Act.
    runtime.memory().store16(0x09000000u + 0x60472u, 501u);
    place_overlay(runtime, kSlot, "game_task.ovl", 0x00200000u);
    native::overlay_loaded(runtime, kSlot);
    (void)call(runtime, runtime.memory().load32(own + 32u * 4u), kMonster);
    runtime.cpu().gpr[2] = 0u;
    const std::uint32_t alone_again = call(runtime, act, kMonster);
    runtime.memory().store32(kMonster, kTable);
    (void)call(runtime, 0x0A090AE0u, 0u);
    check(alone_again == 0u && runtime.memory().load32(kMonster) == own && call(runtime, act, kMonster) == 5u,
        "monsters: a monster made where one that thought alone was runs the base's Act");
    // A monster whose vtable is not in memory keeps it, and the mod is told.
    place_overlay(runtime, kSlot, "game_task.ovl", 0x00200000u);
    native::overlay_loaded(runtime, kSlot);
    runtime.memory().store32(kMonster, 0x10u);
    (void)call(runtime, 0x0A090AE0u, 0u);
    check(runtime.memory().load32(kMonster) == 0x10u &&
            reported("Quest End",
                "monster lone_rathian: its behaviour cannot be given to its monsters: the base's vtable at 0x00000010 "
                "is not in memory"),
        "monsters: a monster made with a vtable outside memory keeps it, and its mod's page says why");
    // A Tigrex-based kind where the model draw is not as traced keeps the
    // base's hidden parts hidden, and its mod's page says so.
    runtime.memory().store32(0x0889EEC4u, 0u);
    check(monsters::add(runtime, {"plain_tigrex", "tigrex", pack, {'G'}, {'M'}, "test", mod}, nullptr) &&
            reported("Quest End",
                "monster plain_tigrex: the model draw is not as traced; the base's hidden parts stay hidden"),
        "monsters: a model draw not as traced is reported on the mod's page");
    // A picture of 17 colours: the base's is shown, and the mod's page says why.
    std::vector<std::uint8_t> colourful(42u * 42u * 4u);
    for (std::uint32_t i = 0; i < 17u; ++i) std::memcpy(colourful.data() + 4u * i, &i, 4u);
    tables::write_traced_code_for_tests(runtime, "monster.pictures");
    const std::optional<std::uint8_t> colourful_kind =
        monsters::add(runtime, {"colourful_tigrex", "tigrex", pack, {'G'}, {'M'}, "test", mod, "", colourful}, nullptr);
    const std::uint32_t pictures = tables::placement("monster.pictures")->address;
    check(colourful_kind &&
            reported("Quest End",
                "monster colourful_tigrex: its picture is not used: its picture has more than 16 colours") &&
            runtime.memory().load32(pictures + 6u * *colourful_kind) == runtime.memory().load32(pictures + 6u * 58u) &&
            runtime.memory().load16(pictures + 6u * *colourful_kind + 4u) ==
                runtime.memory().load16(pictures + 6u * 58u + 4u),
        "monsters: a picture that cannot be used leaves the kind the base's, and its mod's page says why");
    monsters::read_packs_with_for_tests({});
    monsters::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
    tables::reset_for_tests();
}

// A bank of menu textures as traced: three small images, then the image of
// pictures (352 by 352 at 4 bits, 64 palettes of 16 RGBA8888 colours).
std::vector<std::uint8_t> picture_bank() {
    std::vector<std::uint8_t> bank;
    const auto u32 = [&bank](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) bank.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    for (const char c : std::string_view(".TMH0.14")) bank.push_back(static_cast<std::uint8_t>(c));
    u32(6u);
    u32(0u);
    for (int i = 0; i < 3; ++i) {
        u32(16u);
        u32(0u);
        u32(1u);
        u32(0u);
    }
    constexpr std::uint32_t kPixels = 16u + 352u * 352u / 2u, kPalette = 16u + 1024u * 4u;
    u32(16u + kPixels + kPalette);
    u32(0u);
    u32(1u);
    u32(1u);
    u32(kPixels);
    u32(1u);
    u32(4u);
    u32(352u | (352u << 16));
    bank.resize(bank.size() + 352u * 352u / 2u, 0x11u);
    u32(kPalette);
    u32(2u);
    u32(3u);
    u32(1024u);
    bank.resize(bank.size() + 1024u * 4u, 0xEEu);
    return bank;
}

void test_pictures(psprecomp::Runtime &runtime) {
    namespace pictures = mhp3rd::mods::pictures;
    namespace tables = mhp3rd::mods::tables;
    namespace added = mhp3rd::mods::added;
    tables::reset_for_tests();
    added::reset_for_tests();
    pictures::reset_for_tests();
    const std::vector<std::uint8_t> bank = picture_bank();
    // Five colours: a frame of white round red, green, blue and black rows.
    pictures::Pixels picture(42u * 42u * 4u);
    const std::array<std::uint32_t, 5> colours{0xFFFFFFFFu, 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF000000u};
    for (std::size_t p = 0; p < 42u * 42u; ++p) {
        const std::size_t x = p % 42u, y = p / 42u;
        const std::uint32_t c = x == 0u || y == 0u || x == 41u || y == 41u ? colours[0] : colours[1u + y % 4u];
        std::memcpy(picture.data() + 4u * p, &c, 4u);
    }

    // Composing: the picture in its cell, swizzled, its colours in its palette.
    constexpr std::size_t kData = 16u + 3u * 16u + 32u;
    constexpr std::size_t kPaletteData = 16u + 3u * 16u + 16u + 16u + 352u * 352u / 2u + 16u;
    const std::optional<std::vector<std::uint8_t>> out = pictures::compose(bank, 7, 2, 62, picture);
    const auto colour = [&out](int palette, int index) {
        std::uint32_t c = 0u;
        std::memcpy(&c, out->data() + kPaletteData + (static_cast<std::size_t>(palette) * 16u + index) * 4u, 4u);
        return c;
    };
    // Pixel (0, 0) of cell (7, 2) is (294, 84) in the image: byte 147 of row 84,
    // in block 9 of block row 10, the low half; (1, 0) its high half; (41, 41)
    // the high half of byte 7, row 5 of block 10 in block row 15.
    check(out && out->size() == bank.size() && ((*out)[kData + 119u * 128u + 4u * 16u + 3u] & 0x0Fu) == 0u &&
            ((*out)[kData + 119u * 128u + 4u * 16u + 3u] >> 4) == 0u &&
            ((*out)[kData + 175u * 128u + 5u * 16u + 7u] >> 4) == 0u &&
            ((*out)[kData + 175u * 128u + 5u * 16u + 7u] & 0x0Fu) == 0u,
        "pictures: the picture's pixels go to its cell, swizzled as the GE reads them");
    // Pixel (1, 1) has the second colour to appear, (1, 2) the third.
    check(out && ((*out)[kData + 119u * 128u + 5u * 16u + 3u] >> 4) == 1u &&
            ((*out)[kData + 119u * 128u + 6u * 16u + 3u] >> 4) == 2u && (*out)[kData] == 0x11u &&
            (*out)[kData + 119u * 128u + 4u * 16u + 2u] == 0x11u,
        "pictures: each pixel is its colour's index, and the image round the cell is left alone");
    check(out && colour(62, 0) == colours[0] && colour(62, 1) == colours[2] && colour(62, 4) == colours[1] &&
            colour(62, 15) == colours[1] && colour(61, 0) == 0xEEEEEEEEu && colour(63, 0) == 0xEEEEEEEEu,
        "pictures: its colours fill its palette in the order they appear, the last repeated, other palettes kept");
    pictures::Pixels many = picture;
    for (std::uint32_t i = 0; i < 17u; ++i) std::memcpy(many.data() + 4u * (43u + i), &i, 4u);
    std::vector<std::uint8_t> not_tmh = bank;
    not_tmh[1] = 'X';
    std::vector<std::uint8_t> other_format = bank;
    other_format[16u + 48u + 24u] = 5u;
    check(!pictures::compose(not_tmh, 7, 2, 62, picture) && !pictures::compose(other_format, 7, 2, 62, picture) &&
            !pictures::compose(bank, 8, 2, 62, picture) && !pictures::compose(bank, 7, -1, 62, picture) &&
            !pictures::compose(bank, 7, 2, 64, picture) && !pictures::compose(bank, 7, 2, 62, many) &&
            !pictures::compose(bank, 7, 2, 62, pictures::Pixels(16u)) &&
            !pictures::compose(std::vector<std::uint8_t>(bank.begin(), bank.begin() + 200), 7, 2, 62, picture),
        "pictures: a bank not as traced, a cell or palette past the image's, 17 colours or another size compose "
        "nothing");

    // A PNG of the picture reads back as its pixels; another size does not.
    const auto png = [](const pictures::Pixels &pixels, int side) {
        int length = 0;
        unsigned char *bytes = stbi_write_png_to_mem(pixels.data(), side * 4, side, side, 4, &length);
        std::vector<std::uint8_t> file(bytes, bytes + length);
        STBIW_FREE(bytes);
        return file;
    };
    check(pictures::from_png(png(picture, 42)) == picture &&
            !pictures::from_png(png(pictures::Pixels(40u * 40u * 4u), 40)) &&
            !pictures::from_png(std::vector<std::uint8_t>{'n', 'o'}) && !pictures::from_png({}),
        "pictures: a PNG of 42 by 42 pixels is read, another size or not a PNG is not");

    // The game's table: every cell but (7, 2) taken, palettes 0 to 61.
    std::string why;
    check(!pictures::give(runtime, 75u, 58u, nullptr, &why) && why.find("cannot grow") != std::string::npos,
        "pictures: with the game's code not as traced, no kind gets a row");
    tables::write_traced_code_for_tests(runtime, "monster.pictures");
    constexpr std::uint32_t kTable = 0x089FCD5Cu;
    for (std::uint32_t k = 0; k < 69u; ++k) {
        const std::uint32_t cell = k < 23u ? k : k < 63u ? k + 1u : 0u;
        runtime.memory().store16(kTable + 6u * k, static_cast<std::uint16_t>(cell % 8u * 42u));
        runtime.memory().store16(kTable + 6u * k + 2u, static_cast<std::uint16_t>(cell / 8u * 42u));
        runtime.memory().store16(kTable + 6u * k + 4u, static_cast<std::uint16_t>(std::min(k, 61u)));
    }
    const auto row = [&runtime](std::uint32_t kind) {
        const std::uint32_t at = tables::placement("monster.pictures")->address + 6u * kind;
        return std::array<std::uint16_t, 3>{
            runtime.memory().load16(at), runtime.memory().load16(at + 2u), runtime.memory().load16(at + 4u)};
    };
    check(pictures::give(runtime, 75u, 58u, nullptr, &why) && tables::placement("monster.pictures")->rows == 76u &&
            row(75u) == row(58u) && row(69u) == row(0u) && row(74u) == row(0u),
        "pictures: a kind without a picture takes its base's, the kinds before it the game's unknown one");
    check(pictures::serve(runtime, pictures::kTextureFile) == nullptr,
        "pictures: the bank is the game's while no kind has a picture of its own");
    const pictures::Pixels small(5u);
    check(!pictures::give(runtime, 76u, 58u, &many, &why) && why == "its picture has more than 16 colours" &&
            !pictures::give(runtime, 76u, 58u, &small, &why) && why == "its picture is not 42 by 42 pixels",
        "pictures: a picture of more than 16 colours or another size is refused");
    loader_stand_ins(runtime);
    pictures::read_files_with_for_tests([&bank](mhp3rd::mods::FileId file) -> std::optional<std::vector<std::uint8_t>> {
        if (file != pictures::kTextureFile) return std::nullopt;
        return bank;
    });
    check(pictures::give(runtime, 76u, 58u, &picture, &why) && row(76u) == std::array<std::uint16_t, 3>{294u, 84u, 62u},
        "pictures: a picture of its own takes the cell and the palette no other uses");
    const auto served = pictures::serve(runtime, pictures::kTextureFile);
    check(served && *served == *out && pictures::serve(runtime, pictures::kTextureFile) == served &&
            pictures::serve(runtime, 0x0FEBu) == nullptr,
        "pictures: the bank is served with the picture in it, made once, and no other file");
    runtime.cpu().gpr[5] = pictures::kTextureFile;
    check(call(runtime, 0x08864E8Cu, 0x08B3F040u) == bank.size(), "pictures: the game's loader is given that bank");
    // Another mod's change to the bank (a replacement, a patch) is kept.
    std::vector<std::uint8_t> changed = bank;
    changed[kData] = 0x22u;
    pictures::read_files_with_for_tests(
        [&changed](mhp3rd::mods::FileId file) -> std::optional<std::vector<std::uint8_t>> {
            if (file != pictures::kTextureFile) return std::nullopt;
            return changed;
        });
    const auto remade = pictures::serve(runtime, pictures::kTextureFile);
    check(remade && remade != served && (*remade)[kData] == 0x22u &&
            ((*remade)[kData + 119u * 128u + 5u * 16u + 3u] >> 4) == 1u,
        "pictures: a bank another mod changed is made again, with its change and the picture");
    check(
        !pictures::give(runtime, 77u, 58u, &picture, &why) && why == "the pictures' image has no cell or palette left",
        "pictures: once the cells are taken, another picture is refused");
    pictures::reset_for_tests();
    pictures::read_files_with_for_tests(
        [&not_tmh](mhp3rd::mods::FileId) { return std::optional<std::vector<std::uint8_t>>(not_tmh); });
    tables::reset_for_tests();
    tables::write_traced_code_for_tests(runtime, "monster.pictures");
    check(pictures::give(runtime, 75u, 58u, &picture, &why) &&
            pictures::serve(runtime, pictures::kTextureFile) == nullptr,
        "pictures: a bank not as traced is served as the game's");
    pictures::reset_for_tests();
    tables::reset_for_tests();
    added::reset_for_tests();
}

// The log on the Mods page keeps its problems however much is logged.
void test_report_log() {
    namespace native = mhp3rd::mods::native;
    native::reset_for_tests();
    ymod_mod *chatty = native::add_mod("chatty", "Chatty", ".");
    for (int i = 0; i < 5; ++i) native::host()->log(chatty, YMOD_LOG_WARNING, ("warning " + std::to_string(i)).c_str());
    for (int i = 0; i < 2000; ++i) native::host()->log(chatty, YMOD_LOG_INFO, "chatter");
    check(native::reports().size() == 1000u && logged("warning 0") && logged("warning 4"),
        "native: lines that are not problems do not push problems out of the log");
    for (int i = 0; i < 1000; ++i) native::host()->log(chatty, YMOD_LOG_ERROR, ("error " + std::to_string(i)).c_str());
    native::host()->log(chatty, YMOD_LOG_INFO, "one more line");
    check(native::reports().size() == 1000u && logged("error 0") && !logged("one more line"),
        "native: with only problems kept, a line that is not one takes none's place");
    native::reset_for_tests();
}

// The map's picture an area gives, and its place on it: served as the map's
// collision pack's entry 0, its numbers in game_sub's and game_task's tables
// while the quest is played, and the disc's back afterwards.
void test_map_pictures(psprecomp::Runtime &runtime) {
    namespace maps = mhp3rd::mods::maps;
    namespace game = mhp3rd::game;
    using Bytes = std::vector<std::uint8_t>;
    const auto bytes = [](std::string_view s) { return Bytes(s.begin(), s.end()); };
    const auto entry = [](const Bytes &pack, std::size_t i) {
        const auto entries = game::read_pack(pack);
        return entries && i < entries->size() ? Bytes((*entries)[i].begin(), (*entries)[i].end()) : Bytes{};
    };
    const auto put = [](Bytes &out, std::size_t at, std::uint32_t value) { std::memcpy(out.data() + at, &value, 4u); };
    const auto bits = [](float value) { return std::bit_cast<std::uint32_t>(value); };
    // A picture laid out as the game's: `count` images of 160 x 168, 4-bit,
    // 16 RGBA8888 colours, their pixels `fill`.
    const auto picture = [&put](std::uint32_t count, std::uint8_t fill) {
        Bytes out(maps::kPictureHeaderSize + count * maps::kPictureImageSize, fill);
        std::memcpy(out.data(), ".TMH0.14", 8u);
        put(out, 8u, count);
        put(out, 12u, 0u);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t at = 16u + i * maps::kPictureImageSize;
            const std::uint32_t words[] = {13552u, 0u, 1u, 1u, 13456u, 1u, 4u, 160u | 168u << 16u};
            for (std::size_t w = 0; w < 8u; ++w) put(out, at + 4u * w, words[w]);
            const std::uint32_t palette[] = {80u, 2u, 3u, 16u};
            for (std::size_t w = 0; w < 4u; ++w) put(out, at + 13472u + 4u * w, palette[w]);
        }
        return out;
    };
    check(picture(1u, 0u).size() == 13568u && picture(2u, 0u).size() == 27120u &&
            !maps::picture_problem(picture(1u, 0x11u)) && !maps::picture_problem(picture(2u, 0x22u)),
        "map pictures: one or two images laid out as the game's, 13568 or 27120 bytes, are a picture");
    Bytes three = picture(1u, 0u);
    put(three, 8u, 3u);
    Bytes short_one = picture(1u, 0u);
    short_one.pop_back();
    Bytes wide = picture(1u, 0u);
    put(wide, 16u + 28u, 256u | 168u << 16u);
    Bytes eight_bit = picture(2u, 0u);
    put(eight_bit, 16u + maps::kPictureImageSize + 24u, 5u);
    Bytes palette = picture(1u, 0u);
    put(palette, 16u + 13472u + 12u, 256u);
    check(maps::picture_problem(bytes("PNG")) == "its map picture is not a TMH" &&
            maps::picture_problem(three) ==
                "its map picture is not one or two images of 13568 or 27120 bytes in all, as the game's are" &&
            maps::picture_problem(short_one) == maps::picture_problem(three) &&
            maps::picture_problem(wide) ==
                "its map picture's image 1 is not 160 by 168 pixels of 4-bit indices into 16 RGBA8888 colours" &&
            maps::picture_problem(eight_bit) ==
                "its map picture's image 2 is not 160 by 168 pixels of 4-bit indices into 16 RGBA8888 colours" &&
            maps::picture_problem(palette) == maps::picture_problem(wide),
        "map pictures: something not a TMH, three images, a byte short, another size, 8-bit indices or 256 colours "
        "are not");

    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
    loader_stand_ins(runtime);
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "yakumo_map_pictures_test";
    std::filesystem::create_directories(folder);
    const auto write = [&folder](const char *name, const Bytes &data) {
        std::ofstream(folder / name, std::ios::binary)
            .write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
    };
    write("model.pmo",
        bytes(std::string_view("pmo\0"
                               "102\0MINE",
            12)));
    write("ge.bin", bytes("GE"));
    write("tex.tmh", bytes(".TMH0.14 mine"));
    write("ground.bin", bytes("4.00 ground"));
    write("walls.bin", bytes("4.00 walls"));
    const Bytes castle_picture = picture(1u, 0x5Au);
    write("map.tmh", castle_picture);
    write("other.tmh", picture(1u, 0x6Bu));
    write("two.tmh", picture(2u, 0x7Cu));
    write("bad.tmh", three);

    // The disc's game_sub and game_task, as they load: the arena's frame
    // (40000 by 40000 from -20000), its two areas' records (the camp at
    // (-6100, 6900), the floor at (4000, -2500)), its tent (57, 99); map 9's
    // two areas, its picture with two images.
    constexpr std::uint32_t kSub = 0x0A001780u, kTask = 0x0A05E600u;
    constexpr std::uint32_t kFrame = 0x0A044E0Cu + 20u * 8u, kRecords = 0x0A04514Cu, kTent = 0x0A19D1E8u + 4u * 8u;
    const auto overlay = [&put](std::uint32_t load, std::string_view name, std::size_t size) {
        Bytes out(size, 0u);
        std::memcpy(out.data(), "MWo3", 4u);
        put(out, 8u, load);
        put(out, 12u, static_cast<std::uint32_t>(size - 64u));
        std::memcpy(out.data() + 32u, name.data(), name.size());
        return out;
    };
    Bytes sub = overlay(kSub, "game_sub.ovl", 0x0A045800u - kSub);
    for (const auto &[at, value] : std::initializer_list<std::pair<std::uint32_t, std::uint32_t>>{
             {kFrame, bits(40000.0f)}, {kFrame + 4u, bits(40000.0f)}, {kFrame + 8u, bits(-20000.0f)},
             {kFrame + 12u, bits(-20000.0f)}, {kFrame + 16u, kRecords}, {kRecords, bits(-6100.0f)},
             {kRecords + 4u, bits(6900.0f)}, {kRecords + 24u, bits(4000.0f)}, {kRecords + 28u, bits(-2500.0f)},
             {kFrame + 20u, bits(35000.0f)}, {kFrame + 24u, bits(35000.0f)}, {kFrame + 28u, bits(-17500.0f)},
             {kFrame + 32u, bits(-17500.0f)}, {kFrame + 36u, kRecords + 48u}})
        put(sub, at - kSub, value);
    sub[0x0A0457ECu + 8u - kSub] = 2u; // the arena's areas
    sub[0x0A0457ECu + 9u - kSub] = 2u; // map 9's
    Bytes task = overlay(kTask, "game_task.ovl", kTent + 64u - kTask);
    put(task, kTent - kTask, 57u | 99u << 16u);
    const Bytes area_pack = game::write_pack({bytes("THEIR MODEL"), bytes("SCENE"), {}, bytes("THEIR TMH")});
    const Bytes hits_pack = game::write_pack({picture(1u, 0x01u), bytes("CAMP 1"), bytes("CAMP 2"), bytes("FLOOR 1"),
        bytes("FLOOR 2"), bytes("CAMP POINTS"), bytes("FLOOR POINTS")});
    const Bytes ninth_pack =
        game::write_pack({picture(2u, 0x02u), bytes("A"), bytes("B"), bytes("C"), bytes("D"), bytes("E"), bytes("F")});
    bool discs_readable = true;
    const auto reader = [&](mhp3rd::mods::FileId file) -> std::optional<Bytes> {
        if (file == 126u) return discs_readable ? std::optional(sub) : std::nullopt;
        if (file == 123u) return discs_readable ? std::optional(task) : std::nullopt;
        if (file == 0x1389u || file == 0x138Bu || file == 0x1391u || file == 0x1393u) return area_pack;
        if (file == 0x138Au || file == 0x138Cu || file == 0x1390u || file == 0x1392u) return Bytes(0x100u, 0u);
        if (file == 0x1262u) return hits_pack;
        if (file == 0x1263u) return ninth_pack;
        return std::nullopt;
    };
    maps::read_files_with_for_tests(reader);

    ymod_mod *mod = mhp3rd::mods::native::add_mod("test.pictures", "Picture test", folder.string());
    const auto *service = static_cast<const ymod_maps *>(mhp3rd::mods::native::host()->service(mod, YMOD_MAPS, 1u));
    const auto *quests = static_cast<const ymod_quests *>(mhp3rd::mods::native::host()->service(mod, YMOD_QUESTS, 1u));
    // The castle's numbers: 200 by 225 units a pixel, its x 0, z 0 at (78, 97);
    // the camp's at (3, 70), its tent at (33, 98).
    const ymod_area_map castle_map{sizeof(ymod_area_map), "map.tmh", 200.0f, 225.0f, 78.0f, 97.0f, -5, 900};
    const ymod_area_map camp_map{sizeof(ymod_area_map), "map.tmh", 200.0f, 225.0f, 3.0f, 70.0f, 33, 98};
    ymod_area castle{sizeof(ymod_area), "castle", 8, 1, "model.pmo", "ge.bin", "tex.tmh", "ground.bin", "walls.bin"};
    castle.minimap = &castle_map;
    ymod_area camp{sizeof(ymod_area), "camp", 8, 0, "model.pmo", "ge.bin", "tex.tmh", "ground.bin", "walls.bin"};
    camp.minimap = &camp_map;
    check(service != nullptr && service->add_area(mod, &castle) == 0 && service->add_area(mod, &camp) == 0,
        "map pictures: areas give the map's picture and their places on it (the tent read on area 0 only)");
    const auto refused = [&](ymod_area area, ymod_area_map map, const char *id, const std::string &line) {
        area.id = id;
        area.minimap = &map;
        return service != nullptr && service->add_area(mod, &area) == -1 && maps::find(id) == nullptr &&
            logged(line.starts_with("a ymod_area_map") || line.starts_with("area:")
                    ? line
                    : "area " + std::string(id) + " is not added: " + line);
    };
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    const auto with = [&camp_map](auto change) {
        ymod_area_map map = camp_map;
        change(map);
        return map;
    };
    check(refused(camp, with([](ymod_area_map &m) { m.picture = "bad.tmh"; }), "bad_layout",
              "its map picture is not one or two images of 13568 or 27120 bytes in all, as the game's are") &&
            refused(camp, with([](ymod_area_map &m) { m.picture = "none.tmh"; }), "no_picture",
                "area: the picture file none.tmh is not in the mod's folder") &&
            refused(camp, with([](ymod_area_map &m) { m.picture = nullptr; }), "null_picture",
                "area: no picture file given") &&
            refused(camp, with([](ymod_area_map &m) { m.units_x = 0.0f; }), "zero_units",
                "its map's units are not finite numbers above 0") &&
            refused(camp, with([kNaN](ymod_area_map &m) { m.units_z = kNaN; }), "nan_units",
                "its map's units are not finite numbers above 0") &&
            refused(camp, with([](ymod_area_map &m) { m.units_x = -250.0f; }), "negative_units",
                "its map's units are not finite numbers above 0") &&
            refused(camp, with([](ymod_area_map &m) { m.units_z = 3.0e37f; }), "huge_units",
                "its map's units are not finite numbers above 0") &&
            refused(camp, with([kInfinity](ymod_area_map &m) { m.origin_u = kInfinity; }), "far_origin",
                "its place on the map is not a finite number") &&
            refused(camp, with([](ymod_area_map &m) { m.origin_v = 1.0e29f; }), "too_far_origin",
                "its place on the map is not a finite number") &&
            refused(camp, with([](ymod_area_map &m) { m.camp_u = 160; }), "tent_right",
                "its camp's tent is off the map's picture (its pixels are 0 to 159)") &&
            refused(camp, with([](ymod_area_map &m) { m.camp_v = -1; }), "tent_above",
                "its camp's tent is off the map's picture (its pixels are 0 to 159)") &&
            refused(camp, with([](ymod_area_map &m) { m.size = offsetof(ymod_area_map, camp_v); }), "short_map",
                "a ymod_area_map of " + std::to_string(offsetof(ymod_area_map, camp_v)) +
                    " bytes is refused: this call reads " +
                    std::to_string(offsetof(ymod_area_map, camp_v) + sizeof(std::int32_t)) +
                    " (set its size to sizeof(ymod_area_map))"),
        "map pictures: a picture not laid out as the game's or missing, units not finite and above 0, a place not "
        "finite, a camp's tent off the picture and a ymod_area_map too short are refused, with the reason");
    // Boundaries that are taken: the tent at 0 and 159, an origin outside the
    // picture, a ymod_area_map wider than this header's (a later SDK's).
    struct WideMap {
        ymod_area_map map;
        std::uint32_t later[2];
    };
    WideMap wider{camp_map, {0xFFFFFFFFu, 0xFFFFFFFFu}};
    wider.map.size = sizeof(WideMap);
    wider.map.camp_u = 0;
    wider.map.camp_v = 159;
    wider.map.origin_u = -400.0f;
    ymod_area wide_camp = camp;
    wide_camp.id = "wide_camp";
    wide_camp.map = 9;
    wide_camp.minimap = &wider.map;
    // A mod built before the map's picture: its struct ends before
    // `minimap`, which is not read.
    ymod_area older = castle;
    older.id = "older";
    older.size = offsetof(ymod_area, minimap);
    older.minimap = reinterpret_cast<const ymod_area_map *>(std::uintptr_t{1});
    check(service != nullptr && service->add_area(mod, &wide_camp) == 0 && service->add_area(mod, &older) == 0,
        "map pictures: the tent at the picture's edges, a place off the picture, a wider ymod_area_map and an "
        "ymod_area from before the field are taken");

    // A quest's areas give the same picture and units, or none does.
    const ymod_quest_monster monster{"tigrex", 1, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100};
    const char *both[] = {"camp"};
    ymod_quest quest{sizeof(ymod_quest), 62800u, 5, 1, 30u, 0u, 0u, "tigrex", 1u, &monster, 1u, {}, "castle"};
    quest.areas = both;
    quest.area_count = 1u;
    check(quests != nullptr && quests->add(mod, &quest) == 62800, "map pictures: a quest in the castle and the camp");
    const ymod_area_map other_map = with([](ymod_area_map &m) { m.picture = "other.tmh"; });
    const ymod_area_map coarse_map = with([](ymod_area_map &m) { m.units_x = 250.0f; });
    ymod_area other_camp = camp;
    other_camp.id = "other_camp";
    other_camp.minimap = &other_map;
    ymod_area coarse_camp = camp;
    coarse_camp.id = "coarse_camp";
    coarse_camp.minimap = &coarse_map;
    ymod_area bare_camp = camp;
    bare_camp.id = "bare_camp";
    bare_camp.minimap = nullptr;
    check(service->add_area(mod, &other_camp) == 0 && service->add_area(mod, &coarse_camp) == 0 &&
            service->add_area(mod, &bare_camp) == 0,
        "map pictures: camps with another picture, other units and none");
    const auto quest_refused = [&](std::uint32_t id, const char *camp_id, const std::string &line) {
        const char *camps[] = {camp_id};
        ymod_quest q = quest;
        q.id = id;
        q.areas = camps;
        return quests->add(mod, &q) == -1 && logged("quest " + std::to_string(id) + " is not added: " + line);
    };
    check(quest_refused(62801u, "other_camp", "its areas give different map pictures or units") &&
            quest_refused(62802u, "coarse_camp", "its areas give different map pictures or units") &&
            quest_refused(62803u, "bare_camp", "some of its areas give the map's picture and others do not"),
        "map pictures: a quest whose areas give different pictures, other units, or a picture and none, is refused");
    std::string why;
    check(maps::play_quest_in("castle", 62804u) && !maps::play_quest_in("bare_camp", 62804u, &why) &&
            why == "some of its areas give the map's picture and others do not" &&
            !maps::play_quest_in("other_camp", 62804u, &why) && why == "its areas give different map pictures or units",
        "map pictures: and an area is not played with others whose pictures do not go with its own");
    // A quest that leaves some of the map's areas to the game keeps the
    // game's units: 250 a pixel on the arena.
    ymod_quest alone = quest;
    alone.id = 62805u;
    alone.areas = nullptr;
    alone.area_count = 0u;
    check(quests->add(mod, &alone) == -1 &&
            logged("quest 62805 is not added: it leaves some of map 8's areas to the game, whose picture has 250 by "
                   "250 units a pixel, and its areas' have 200 by 225"),
        "map pictures: a quest in the castle alone, whose units are not the game's, is refused");
    const ymod_area_map game_scale{sizeof(ymod_area_map), "map.tmh", 250.0f, 250.0f, 100.0f, 74.0f, 0, 0};
    ymod_area yard = castle;
    yard.id = "yard";
    yard.minimap = &game_scale;
    ymod_quest in_yard = alone;
    in_yard.id = 62806u;
    in_yard.area = "yard";
    check(service->add_area(mod, &yard) == 0 && quests->add(mod, &in_yard) == 62806,
        "map pictures: one at the game's units is added");
    // Map 9's picture has two images: one is refused.
    ymod_area ninth = castle;
    ninth.id = "ninth";
    ninth.map = 9;
    const char *ninth_camp[] = {"wide_camp"};
    ymod_quest on_ninth = quest;
    on_ninth.id = 62807u;
    on_ninth.area = "ninth";
    on_ninth.areas = ninth_camp;
    check(service->add_area(mod, &ninth) == 0 && quests->add(mod, &on_ninth) == -1 &&
            logged("quest 62807 is not added: its map picture has 1 image, map 9's has 2 images"),
        "map pictures: a picture with another number of images than the game's map's is refused");

    // Played: the collision pack's entry 0 is the picture.
    auto &memory = runtime.memory();
    const auto load_overlays = [&](bool with_task) {
        place_overlay(runtime, kSub, "game_sub.ovl", 0x00050000u);
        memory.copy_in(kSub + 64u, std::span<const std::uint8_t>(sub).subspan(64u));
        memory.store16(0x0A0457C4u + 16u, 0x1389u);
        memory.store16(0x0A04579Cu + 16u, 0x138Au);
        if (with_task) {
            place_overlay(runtime, kTask, "game_task.ovl", static_cast<std::uint32_t>(task.size()));
            memory.copy_in(kTask + 64u, std::span<const std::uint8_t>(task).subspan(64u));
        } else {
            place_overlay(runtime, kTask, "lobby_task.ovl", static_cast<std::uint32_t>(task.size()));
        }
    };
    const auto f = [&memory](std::uint32_t at) { return std::bit_cast<float>(memory.load32(at)); };
    load_overlays(true);
    memory.store32(0x08AB3640u, 0x09000000u);
    memory.store16(0x09000000u + 0x60472u, 62800u);
    const auto served = [&](mhp3rd::mods::FileId file) {
        const auto b = maps::serve(runtime, file);
        return b ? *b : Bytes{};
    };
    const Bytes collision = served(0x1262u);
    check(entry(collision, 0) == castle_picture && entry(collision, 1) == bytes("4.00 ground") &&
            entry(collision, 3) == bytes("4.00 ground") && entry(collision, 5) == bytes("CAMP POINTS"),
        "map pictures: while the quest is played, the map's collision pack has the picture as its entry 0");
    // The tables: the frame 160 x 200 by 160 x 225 from the game's x0 and z0,
    // each area's origin x units + x0, the camp's tent.
    maps::overlay_loaded(runtime);
    const bool written = f(kFrame) == 32000.0f && f(kFrame + 4u) == 36000.0f && f(kFrame + 8u) == -20000.0f &&
        f(kFrame + 12u) == -20000.0f && memory.load32(kFrame + 16u) == kRecords &&
        f(kRecords) == 3.0f * 200.0f - 20000.0f && f(kRecords + 4u) == 70.0f * 225.0f - 20000.0f &&
        f(kRecords + 24u) == 78.0f * 200.0f - 20000.0f && f(kRecords + 28u) == 97.0f * 225.0f - 20000.0f &&
        memory.load16(kTent) == 33u && memory.load16(kTent + 2u) == 98u;
    check(written, "map pictures: the map's frame, the areas' places and the camp's tent are written into the tables");
    // The world to the picture as the game takes it: a point of the castle
    // at (x, z) is pixel (78 + x / 200, 97 + z / 225).
    const float u = 160.0f * (1000.0f + f(kRecords + 24u) - f(kFrame + 8u)) / f(kFrame);
    const float v = 160.0f * (-4500.0f + f(kRecords + 28u) - f(kFrame + 12u)) / f(kFrame + 4u);
    check(u == 83.0f && v == 77.0f, "map pictures: and put the castle's (1000, -4500) on its pixel (83, 77)");
    const auto disc_values = [&]() {
        return f(kFrame) == 40000.0f && f(kFrame + 4u) == 40000.0f && f(kRecords) == -6100.0f &&
            f(kRecords + 4u) == 6900.0f && f(kRecords + 24u) == 4000.0f && f(kRecords + 28u) == -2500.0f;
    };
    // Another quest, a game's: the disc's values back, its picture the game's.
    memory.store16(0x09000000u + 0x60472u, 501u);
    maps::overlay_loaded(runtime);
    check(disc_values() && memory.load16(kTent) == 57u && memory.load16(kTent + 2u) == 99u &&
            maps::serve(runtime, 0x1262u) == nullptr,
        "map pictures: on another quest the disc's values are back and the game's collision pack loads");
    // Back on the quest, then the quest ends: game_task goes, game_sub stays.
    memory.store16(0x09000000u + 0x60472u, 62800u);
    maps::overlay_loaded(runtime);
    const bool again = f(kFrame) == 32000.0f && memory.load16(kTent) == 33u;
    load_overlays(false);
    memory.store32(kFrame, bits(32000.0f)); // game_sub stays loaded with Yakumo's values
    memory.store32(kRecords + 24u, bits(78.0f * 200.0f - 20000.0f));
    maps::overlay_loaded(runtime);
    check(
        again && disc_values(), "map pictures: when the quest's game_task goes, game_sub has the disc's values again");
    // The overlays loaded again, from the disc: written again.
    load_overlays(true);
    maps::overlay_loaded(runtime);
    check(f(kFrame) == 32000.0f && f(kRecords + 24u) == 78.0f * 200.0f - 20000.0f && memory.load16(kTent) == 33u,
        "map pictures: overlays loaded again on the quest are written again");
    // A quest in the yard alone: the game's frame kept, its record moved,
    // the camp's (the game's) and its tent kept.
    memory.store16(0x09000000u + 0x60472u, 62806u);
    maps::overlay_loaded(runtime);
    check(f(kFrame) == 40000.0f && f(kRecords + 24u) == 5000.0f && f(kRecords + 28u) == -1500.0f &&
            f(kRecords) == -6100.0f && memory.load16(kTent) == 57u && entry(served(0x1262u), 0) == castle_picture,
        "map pictures: a quest in one area at the game's units moves only that area's record");
    // A quest whose areas give no picture leaves the tables alone.
    ymod_area plain = castle;
    plain.id = "plain";
    plain.minimap = nullptr;
    ymod_quest in_plain = in_yard;
    in_plain.id = 62808u;
    in_plain.area = "plain";
    check(service->add_area(mod, &plain) == 0 && quests->add(mod, &in_plain) == 62808,
        "map pictures: a quest in an area without a picture");
    memory.store16(0x09000000u + 0x60472u, 62808u);
    maps::overlay_loaded(runtime);
    check(disc_values() && memory.load16(kTent) == 57u && entry(served(0x1262u), 0) == entry(hits_pack, 0),
        "map pictures: keeps the game's picture and tables");
    // The disc's game_sub unreadable when the quest is played: the picture is
    // not served and nothing is written.
    maps::reset_for_tests();
    discs_readable = false;
    maps::read_files_with_for_tests(reader);
    check(service->add_area(mod, &castle) == 0 && service->add_area(mod, &camp) == 0 &&
            maps::play_quest_in("castle", 62809u) && maps::play_quest_in("camp", 62809u),
        "map pictures: a quest whose game files cannot be read when it is added");
    memory.store16(0x09000000u + 0x60472u, 62809u);
    maps::overlay_loaded(runtime);
    check(disc_values() && entry(served(0x1262u), 0) == entry(hits_pack, 0) &&
            entry(served(0x1262u), 3) == bytes("4.00 ground"),
        "map pictures: is played with the game's picture and tables, its areas served");

    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    maps::reset_for_tests();
    mhp3rd::mods::added::reset_for_tests();
}

int main() {
    psprecomp::Runtime runtime(64u * 1024u * 1024u);
    mhp3rd::kernel().install(runtime, 0u, 0x08900000u);
    runtime.register_function(kSentinel, &done, "test_sentinel");
    store(runtime, 0x08A00000u, {0x00801021u, 0x03E00008u, 0u});              // addu v0, a0, zero; jr ra
    store(runtime, 0x08A00100u, {0x24020005u, 0x03E00008u, 0u});              // addiu v0, zero, 5; jr ra
    store(runtime, 0x08A00200u, {0x24090007u, 0x25290001u, 0x03E00008u, 0u}); // t1 = 7; t1 += 1; jr ra

    // The template mod hooks armor.model_of_part by name. This memory holds a
    // stand-in function there, not the game's, so the mods start with a table
    // that does not check its bytes; a hook over bytes that differ is refused.
    namespace symbols = mhp3rd::mods::symbols;
    std::vector<symbols::Symbol> unchecked = symbols::all();
    for (symbols::Symbol &symbol : unchecked)
        if (symbol.name == "armor.model_of_part") symbol.check.reset();
    symbols::use_table_for_tests(unchecked);
    store(runtime, 0x08869778u, {0x00801021u, 0x03E00008u, 0u}); // addu v0, a0, zero; jr ra

    // Built next to this test; on a device, pushed into its working directory.
    const auto built = [](std::filesystem::path path) {
        return std::filesystem::is_regular_file(path) ? path : std::filesystem::current_path() / path.filename();
    };
    mhp3rd::mods::native::start(runtime,
        {{"bad", "/nonexistent/library.so", {}}, {"test", built(MHP3RD_NATIVE_TEST_MOD), {}},
            {"template", built(MHP3RD_TEMPLATE_TEST_MOD), {}}, {"future", built(MHP3RD_FUTURE_TEST_MOD), {}}});
    symbols::reset_for_tests();
    check(logged_starting("cannot load library.so: "), "native: a library that cannot be loaded is reported, and why");
    check(logged("started"), "native: the test mod starts");
    check(logged("needs a newer Yakumo (mod interface 2, this Yakumo has 1)"),
        "native: a mod made for a newer interface is refused before it runs");
    check(logged("memory ok") && logged("bad address refused"), "native: guest memory, checked");
    check(logged("setting=1"), "native: a setting without saved choices has its fallback");
    check(logged("unknown service absent"), "native: an unknown service is null");
    check(logged("logging armor models"), "native: the SDK's template mod starts and hooks its function");
    check(logged("symbol armor.model_of_part 08869778") && logged("unknown symbol absent") &&
            logged("symbol by address") && logged("symbols listed"),
        "native: the game's functions by name and by address");

    const std::uint32_t v0 = call(runtime, 0x08A00000u, 10u);
    check(logged("before a0=11") && v0 == 11u, "native: a BEFORE hook runs first and changes the arguments");
    check(logged("after v0=11") && runtime.cpu().pc == kSentinel, "native: an AFTER hook sees the result and returns");
    check(call(runtime, 0x08A00100u, 0u) == 0x99u && runtime.cpu().pc == kSentinel,
        "native: a REPLACE hook returns its own value to the caller");
    (void)call(runtime, 0x08A00200u, 0u);
    check(logged("instruction t1=7") && runtime.cpu().gpr[9] == 8u, "native: an instruction hook runs mid-function");

    // A host feature takes the hooked function over, then gives it back.
    runtime.register_function(0x08A00000u, &host_feature, "mhp3rd_test_feature");
    mhp3rd::mods::native::frame(runtime);
    check(call(runtime, 0x08A00000u, 10u) == 22u && logged("after v0=22"),
        "native: hooks go back on top of a host feature that replaces the function");
    runtime.register_function(0x08A00000u, &host_feature, "test_generated_original");
    mhp3rd::mods::native::frame(runtime);
    check(call(runtime, 0x08A00000u, 10u) == 11u && runtime.cpu().pc == kSentinel,
        "native: once the host gives the function back, the hooks run the game's code again");

    mhp3rd::mods::native::frame(runtime);
    check(logged("get=1"), "native: a setting the player never changed reads as its fallback");
    check(logged("game started"), "native: the game-started event comes once, at the first frame");
    check(mhp3rd::mods::native::running() >= 1u, "native: the mod counts as running");

    mhp3rd::mods::native::file_loaded(0x601u);
    check(!logged("file event after unsubscribe"), "native: an event taken back is not delivered");

    test_added_files(runtime);
    test_events(runtime);
    test_ui();
    test_sizes();
    test_symbols(runtime);
    test_hook_changes(runtime);
    test_host_returns(runtime);
    test_shared_overlay_address(runtime);
    test_quests(runtime);
    test_tables(runtime);
    test_monsters(runtime);
    test_monster_quest_end(runtime);
    test_equipment(runtime);
    test_maps(runtime);
    test_map_points(runtime);
    test_map_gathering(runtime);
    test_map_slots(runtime);
    test_map_pictures(runtime);
    test_pictures(runtime);
    test_report_log();

    std::printf("%s\n", failures == 0 ? "all native mod checks passed" : "native mod checks FAILED");
    return failures == 0 ? 0 : 1;
}
