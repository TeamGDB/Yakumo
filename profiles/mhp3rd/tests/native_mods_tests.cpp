// Native mods through the real loader: the test mod (native_test_mod.cpp)
// is loaded from its shared library, takes hooks on synthetic guest
// functions, and logs what it sees; the guest code runs through the JIT.
#include "mods/mod_library.hpp"
#include "mods/mhp3rd_added_files.hpp"
#include "mods/mhp3rd_equipment.hpp"
#include "mods/mhp3rd_maps.hpp"
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mhp3rd_monsters.hpp"
#include "mods/mod_native.hpp"
#include "mods/mhp3rd_quests.hpp"
#include "mods/mhp3rd_symbols.hpp"
#include "mods/mhp3rd_tables.hpp"

#include "yakumo/mod.h"

#include "game/game_data.hpp"
#include "game/guest_ram.hpp"
#include "game/pack_file.hpp"
#include "game/quest_builder.hpp"
#include "game/quest_file.hpp"
#include "kernel/kernel.hpp"

#include "psprecomp/jit.hpp"
#include "psprecomp/runtime.hpp"

#include <bit>
#include <cmath>
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
    const std::size_t before_withdraw = native::running();
    native::withdraw(failing);
    native::frame(runtime);
    (void)call(runtime, 0x08A00300u, 4u);
    check(g_added_seen == 0 && services->import(g_self, "test.failing", 1u) == nullptr &&
            native::panels("Failing", false).empty() && native::running() == before_withdraw - 1u,
        "native: a withdrawn mod's hooks, events, services and panels are gone");
    check(g_hooks->function(failing, nullptr, 0x08A00300u, YMOD_HOOK_BEFORE, &count_added, nullptr) == 0u &&
            events->subscribe(
                failing, YMOD_EVENT_FRAME, [](int, std::uint32_t, void *) {}, nullptr) != 0,
        "native: a withdrawn mod cannot register again");

    // A mod that logs every frame does not grow the log without end.
    for (int line = 0; line < 1500; ++line) native::host()->log(g_self, YMOD_LOG_INFO, "chatter");
    check(native::reports().size() <= 1000u && logged("needs a newer Yakumo (mod interface 2, this Yakumo has 1)"),
        "native: the log keeps its latest lines and every problem");
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
    const Bytes hits_pack = game::write_pack(
        {bytes("MAP TMH"), bytes("CAMP 1"), bytes("CAMP 2"), bytes("FLOOR 1"), bytes("FLOOR 2"), bytes("OTHER")});
    maps::read_files_with_for_tests([&](mhp3rd::mods::FileId file) -> std::optional<Bytes> {
        if (file == 0x138Bu) return area_pack;
        if (file == 0x1262u) return hits_pack;
        if (file == 0x1263u) return bytes("not a pack");
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
            entry(collision, 5) == bytes("OTHER"),
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
    check(
        maps::serve(runtime, 0x1263u) == nullptr, "maps: a collision pack that is not as expected is left the game's");

    // A quest played in an area names it; one that names an area not added is refused.
    const auto *quests = static_cast<const ymod_quests *>(mhp3rd::mods::native::host()->service(mod, YMOD_QUESTS, 1u));
    const ymod_quest_monster monster{"tigrex", 1, {0.0f, 0.0f, 0.0f}, 0x1E, 0x13, 100};
    ymod_quest quest{sizeof(ymod_quest), 62603u, 5, 1, 30u, 0u, 0u, "tigrex", 1u, &monster, 1u, {}, "nowhere"};
    check(quests != nullptr && quests->add(mod, &quest) == -1 &&
            logged("quest 62603 is not added: its area \"nowhere\" has not been added"),
        "maps: a quest naming an area that was not added is refused");

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
    monsters::read_packs_with_for_tests([&base](mhp3rd::mods::FileId) { return std::optional(base); });
    ymod_mod *mod = mhp3rd::mods::native::add_mod("test.monsters", "Monster test", folder.string());
    const auto *service =
        static_cast<const ymod_monsters *>(mhp3rd::mods::native::host()->service(mod, YMOD_MONSTERS, 1u));
    ymod_monster spec{
        sizeof(ymod_monster), "black_fatalis", "tigrex", "model/model.pac", "model/ge.bin", "model/motions.bin"};
    check(service != nullptr && service->add(mod, &spec) == 76 && service->kind("black_fatalis") == 76 &&
            service->kind("tigrex") == 58 && service->kind("nothing") == -1 && service->kind(nullptr) == -1 &&
            std::string(service->id_of(76)) == "black_fatalis" && service->id_of(255) == nullptr &&
            service->id_of(-1) == nullptr,
        "monsters: the service adds a kind from the mod's files and names kinds");
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
    check(service != nullptr && service->add(mod, &unknown) == -1 &&
            logged("monster unknown is not added: the base \"kirin\" is not one of the game's kinds"),
        "monsters: the service says why a kind is not added");
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
    } acting{service};
    check(service != nullptr && service->play_motion(kMonster, 3, 4) == -1,
        "monsters: a motion is played only from a part of behaviour");
    const ymod_monster_fn act = [](ymod_cpu *, std::uint32_t monster, void *user) -> int {
        auto *a = static_cast<Acting *>(user);
        if (a->service->play_motion(monster, 0x10012, 4) != -1 || a->service->play_motion(monster, 12, -1) != -1)
            return YMOD_RETURN;
        (void)a->service->play_motion(monster, 12, 4);
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
        if (a->service->attack(monster, 256, 6) != -1 || a->service->attack(monster, 1, -1) != -1 ||
            a->service->end_attack(monster, -1) != -1)
            return YMOD_CONTINUE;
        (void)a->service->attack(monster, 2, 23);
        (void)a->service->end_attack(monster, 1);
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
    runtime.memory().store32(kAttacked + 12u, 0u);
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
    check(service != nullptr && service->attack_shape(mod, 58, 2, jaws, 2) == -1 &&
            service->attack_shape(mod, 78, 32, jaws, 2) == -1 && service->attack_shape(mod, 78, 2, jaws, 0) == -1 &&
            service->attack_shape(mod, 78, 2, nullptr, 2) == -1 && service->attack_shape(mod, 78, 2, broken, 1) == -1 &&
            service->attack_shape(mod, 78, 2, flat, 1) == -1 &&
            logged("monster 78 is not given attack shape 2: a sphere needs a joint, a radius above 0 and a centre"),
        "monsters: attack shapes only for the mod's kind, a shape that exists and usable spheres");
    check(service != nullptr && service->attack_shape(mod, 78, 2, jaws, 2) == 0,
        "monsters: a kind the mod added takes an attack shape of its own");
    const std::uint32_t attacked_v0 = call(runtime, attacking_stub, kMonster);
    check(attacked_v0 != 7u && runtime.memory().load32(kAttacked) == 2u &&
            runtime.memory().load32(kAttacked + 4u) == 23u && runtime.memory().load32(kAttacked + 8u) == 0u &&
            runtime.memory().load32(kAttacked + 12u) == 4u && runtime.memory().load32(kAttacked + 16u) == kMonster &&
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
    check(service != nullptr && service->body_shapes(mod, 58, body, 2) == -1 &&
            service->body_shapes(mod, 78, body, 0) == -1 && service->body_shapes(mod, 78, nullptr, 2) == -1 &&
            service->body_shapes(mod, 78, thin, 1) == -1 &&
            logged(
                "monster 78 is not given body capsules: a capsule needs joints, a part, a radius above 0 and its ends"),
        "monsters: body capsules only for the mod's kind, 1 to 64 of them, each usable");
    runtime.memory().store32(kMonster + 0xD50u, kKindData);
    check(service != nullptr && service->body_shapes(mod, 78, body, 2) == 0,
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
    check(service != nullptr && service->footsteps(mod, 58, steps, 3) == -1 &&
            service->footsteps(mod, 78, steps, 0) == -1 && service->footsteps(mod, 78, nullptr, 3) == -1 &&
            service->footsteps(mod, 78, huge, 1) == -1 && service->footsteps(mod, 78, late, 1) == -1 &&
            logged("monster 78 is not given footsteps: a footstep needs a motion below 1000, a frame, a joint and a "
                   "size from 0 to 9"),
        "monsters: footsteps only for the mod's kind, 1 to 256 of them, each usable");
    runtime.memory().store32(kAttacked + 0x2Cu, 0u);
    check(service != nullptr && service->footsteps(mod, 78, steps, 3) == 0,
        "monsters: a kind the mod added takes footsteps of its own");
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
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    monsters::reset_for_tests();
    added::reset_for_tests();
    tables::reset_for_tests();
}

int main() {
    psprecomp::Runtime runtime(64u * 1024u * 1024u);
    mhp3rd::kernel().install(runtime, 0u, 0x08900000u);
    runtime.register_function(kSentinel, &done, "test_sentinel");
    store(runtime, 0x08A00000u, {0x00801021u, 0x03E00008u, 0u});              // addu v0, a0, zero; jr ra
    store(runtime, 0x08A00100u, {0x24020005u, 0x03E00008u, 0u});              // addiu v0, zero, 5; jr ra
    store(runtime, 0x08A00200u, {0x24090007u, 0x25290001u, 0x03E00008u, 0u}); // t1 = 7; t1 += 1; jr ra

    // Built next to this test; on a device, pushed into its working directory.
    const auto built = [](std::filesystem::path path) {
        return std::filesystem::is_regular_file(path) ? path : std::filesystem::current_path() / path.filename();
    };
    mhp3rd::mods::native::start(runtime,
        {{"bad", "/nonexistent/library.so", {}}, {"test", built(MHP3RD_NATIVE_TEST_MOD), {}},
            {"template", built(MHP3RD_TEMPLATE_TEST_MOD), {}}, {"future", built(MHP3RD_FUTURE_TEST_MOD), {}}});
    check(logged("cannot load library.so: ") || mhp3rd::mods::native::reports().front().problem,
        "native: a library that cannot be loaded is reported");
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
    test_symbols(runtime);
    test_hook_changes(runtime);
    test_quests(runtime);
    test_tables(runtime);
    test_monsters(runtime);
    test_equipment(runtime);
    test_maps(runtime);

    std::printf("%s\n", failures == 0 ? "all native mod checks passed" : "native mod checks FAILED");
    return failures == 0 ? 0 : 1;
}
