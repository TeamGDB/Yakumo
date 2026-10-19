// Native mods through the real loader: the test mod (native_test_mod.cpp)
// is loaded from its shared library, takes hooks on synthetic guest
// functions, and logs what it sees; the guest code runs through the JIT.
#include "mods/mod_library.hpp"
#include "mods/mhp3rd_added_files.hpp"
#include "mods/mod_native.hpp"
#include "mods/mhp3rd_quests.hpp"
#include "mods/mhp3rd_symbols.hpp"

#include "yakumo/mod.h"

#include "game/guest_ram.hpp"
#include "game/quest_file.hpp"
#include "kernel/kernel.hpp"

#include "psprecomp/jit.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdio>
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
void test_added_files(psprecomp::Runtime &runtime) {
    namespace added = mhp3rd::mods::added;
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
    payload[0x10u + 0x1Cu] = 0x61u; // 60001
    payload[0x10u + 0x1Du] = 0xEAu;
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
            quests::find(60001u) != nullptr && quests::served().size() == 1u,
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
    check(file_for(60001u) == 0x6001u && runtime.cpu().pc == kSentinel, "quests: the mod's quest id loads its file");
    check(file_for(60002u) == 0xFFFFu && runtime.cpu().pc == kSentinel, "quests: another id still finds no file");
    check(mhp3rd::mods::native::running() == mods_before, "quests: Yakumo's own hook is not counted as a mod");
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
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

    std::printf("%s\n", failures == 0 ? "all native mod checks passed" : "native mod checks FAILED");
    return failures == 0 ? 0 : 1;
}
