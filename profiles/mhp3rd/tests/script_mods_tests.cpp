// Script mods: Luau scripts through the real engine, with hooks on synthetic
// guest functions, the sandbox, and the limits on time.
#include "mods/mod_native.hpp"
#include "mods/mod_script.hpp"
#include "mods/mhp3rd_symbols.hpp"

#include "yakumo/mod.h"

#include "kernel/kernel.hpp"

#include "psprecomp/runtime.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

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
        if (report.line.find(text) != std::string::npos) return true;
    return false;
}

void store(psprecomp::Runtime &runtime, std::uint32_t address, std::initializer_list<std::uint32_t> words) {
    for (const std::uint32_t word : words) {
        runtime.memory().store32(address, word);
        address += 4u;
    }
}

std::uint32_t call(psprecomp::Runtime &runtime, std::uint32_t address, std::uint32_t a0) {
    runtime.cpu().gpr[4] = a0;
    runtime.cpu().gpr[31] = kSentinel;
    std::ostringstream quiet;
    std::streambuf *previous = std::cerr.rdbuf(quiet.rdbuf());
    runtime.run(address, 10000u);
    std::cerr.rdbuf(previous);
    return runtime.cpu().gpr[2];
}

fs::path write_script(const fs::path &folder, const std::string &name, const std::string &source) {
    fs::create_directories(folder);
    const fs::path path = folder / name;
    std::ofstream(path, std::ios::binary) << source;
    return path;
}

// Stand-in widgets: every input changes, every button is pressed.
int g_shapes = 0;
void w_text(const char *) {}
void w_text_colored(std::uint32_t, const char *) {}
void w_none() {}
int w_label(const char *) {
    return 1;
}
int w_checkbox(const char *, int *value) {
    *value = 1;
    return 1;
}
int w_slider_int(const char *, std::int32_t *value, std::int32_t, std::int32_t) {
    ++*value;
    return 1;
}
int w_slider_float(const char *, float *value, float, float) {
    *value = 0.25f;
    return 1;
}
int w_input(const char *, char *buffer, std::uint32_t capacity) {
    std::snprintf(buffer, capacity, "%s", "typed text");
    return 1;
}
void w_line(float, float, float, float, std::uint32_t, float) {
    ++g_shapes;
}
void w_rect(float, float, float, float, std::uint32_t, int) {
    ++g_shapes;
}
void w_circle(float, float, float, std::uint32_t, int) {
    ++g_shapes;
}
void w_text_at(float, float, std::uint32_t, const char *) {
    ++g_shapes;
}
const ymod_widgets kWidgets{1u, sizeof(ymod_widgets), &w_text, &w_text_colored, &w_none, &w_none, &w_label, &w_label,
    &w_checkbox, &w_slider_int, &w_slider_float, &w_input, &w_line, &w_rect, &w_circle, &w_text_at};

void place_overlay(psprecomp::Runtime &runtime, std::uint32_t base, const std::string &name) {
    for (std::uint32_t at = 0u; at < 64u; at += 4u) runtime.memory().store32(base + at, 0u);
    runtime.memory().store32(base, 0x336F574Du);
    runtime.memory().store32(base + 8u, base);
    runtime.memory().store32(base + 12u, 0x1000u);
    for (std::size_t i = 0; i < name.size(); ++i)
        runtime.memory().store8(base + 32u + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(name[i]));
}

} // namespace

int main() {
    const fs::path folder = fs::temp_directory_path() /
        ("yakumo_script_tests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    psprecomp::Runtime runtime(64u * 1024u * 1024u);
    mhp3rd::kernel().install(runtime, 0u, 0x08900000u);
    runtime.register_function(kSentinel, &done, "test_sentinel");
    store(runtime, 0x08A00000u, {0x00801021u, 0x03E00008u, 0u});              // addu v0, a0, zero; jr ra
    store(runtime, 0x08A00100u, {0x24020005u, 0x03E00008u, 0u});              // addiu v0, zero, 5; jr ra
    store(runtime, 0x08A00200u, {0x24090007u, 0x25290001u, 0x03E00008u, 0u}); // t1 = 7; t1 += 1; jr ra

    const fs::path main = write_script(folder, "main.luau", R"(
local block = yakumo.allocate(64)
yakumo.write32(block, 0x12345678)
yakumo.log("memory", yakumo.read32(block) == 0x12345678, yakumo.read32(16) == nil)
yakumo.log("ranges", pcall(yakumo.write32, block, 1e30) == false, pcall(yakumo.read32, 0 / 0) == false,
    pcall(yakumo.write32, block, -1) == true and yakumo.read32(block) == 0xFFFFFFFF)
yakumo.log("setting", yakumo.setting_switch("on", "On", true), yakumo.setting_choice("mode", "Mode", {"A", "B"}, 2))
yakumo.log("setting now", yakumo.setting("on"), yakumo.setting("mode"), yakumo.setting("never"),
    yakumo.setting_number("count", "Count", 0, 9, 4), yakumo.setting("count"))
yakumo.log("bad key", yakumo.setting_switch("x\n[mod other]\nenabled", "Bad", true), yakumo.setting("x\n[mod other]\nenabled"))
yakumo.hook(0x08A00000, "before", function(cpu) cpu.a0 = cpu.a0 + 1 end)
local gone = yakumo.hook(0x08A00100, "before", function() yakumo.log("removed hook ran") end)
yakumo.unhook(gone)
local once
once = yakumo.hook(0x08A00000, "before", function() yakumo.log("one-shot ran"); yakumo.unhook(once) end)
yakumo.hook(0x08A00000, "after", function(cpu) yakumo.log("after v0=" .. cpu.v0) end)
yakumo.hook(0x08A00100, "replace", function(cpu) cpu.v0 = 0x99; return true end)
yakumo.hook_instruction(0x08A00204, function(cpu) yakumo.log("instruction t1=" .. cpu.t1) end)
local frames = 0
yakumo.on("frame", function() frames = frames + 1 end)
yakumo.on("start", function() yakumo.log("started, frames so far", frames) end)
yakumo.log("sandbox", io == nil, os.execute == nil and os.remove == nil, pcall(function() string.foo = 1 end) == false)
local kept
yakumo.hook(0x08A00000, "before", function(cpu) kept = cpu end)
yakumo.on("frame", function() if kept then local ok = pcall(function() return kept.a0 end); yakumo.log("stale cpu refused", not ok); kept = nil end end)
local quest_hud, overlay = yakumo.symbol("hud.quest")
yakumo.log("symbols", yakumo.symbol("armor.model_of_part") == 0x08869778, quest_hud == 0x0A17370C, overlay,
    yakumo.symbol("no.such") == nil, yakumo.symbol_name(0x08869778), yakumo.symbol_name(4) == nil)
yakumo.log("unknown hook", yakumo.hook("no.such", "before", function() end) == nil,
    yakumo.hook_instruction("no.such", function() end) == nil)
yakumo.log("overlay at", yakumo.overlay_at(0x08A00000) == nil)
local base, rows, row = yakumo.table("monster.models.second")
local moved = yakumo.extend_table("monster.models.second", 80)
local now, more = yakumo.table("monster.models.second")
yakumo.log("tables", base == 0x08982E64, rows, row, moved ~= nil and moved == now, more, yakumo.table("no.such") == nil,
    yakumo.extend_table("no.such", 3) == nil, yakumo.extend_table("monster.models.second", -1) == nil)
yakumo.log("monsters", yakumo.monster_kind("tigrex"), yakumo.monster_kind("gold_rathian"), yakumo.monster_kind("nothing") == nil,
    yakumo.add_monster({id = "scripted", based_on = "tigrex", model = "none.pac", geometry = "none.bin", motions = "none.bin",
        picture = "../picture.png"}) == nil,
    pcall(yakumo.add_monster, "not a table") == false)
yakumo.on("quest_started", function(id) yakumo.log("quest started", id) end)
yakumo.on("quest_ended", function(id) yakumo.log("quest ended", id) end)
yakumo.on("overlay_unloaded", function(base) yakumo.log("overlay gone", base) end)
yakumo.log("bad event", pcall(yakumo.on, "nothing", function() end) == false)
local kept_ui
yakumo.log("panels", yakumo.panel("Script Panel", function(ui)
    kept_ui = ui
    ui.text("hello")
    ui.text("red", 0xFF0000FF)
    local on, changed = ui.checkbox("Flag", false)
    local n, n_changed = ui.slider("N", 3, 0, 9)
    local f, f_changed = ui.slider_float("F", 0.5, 0, 1)
    local t, t_changed = ui.input("Name", "abc", 8)
    yakumo.log("panel", on, changed, n, n_changed, f, f_changed, t, t_changed, ui.button("Go"), ui.section("S"))
end), yakumo.overlay("Script Overlay", function(ui)
    ui.line(0, 0, 1, 1, 0xFFFFFFFF, 2)
    ui.rect(0, 0, 1, 1)
    ui.circle(0.5, 0.5, 0.1, 0x00FF00FF, true)
    ui.text_at(0.5, 0.5, "x")
    ui.separator()
    ui.same_line()
    yakumo.log("overlay drew")
end))
yakumo.on("frame", function() if kept_ui then yakumo.log("widgets outside a draw refused", pcall(kept_ui.text, "x") == false); kept_ui = nil end end)
)");
    const fs::path loop = write_script(folder, "loop.luau", "yakumo.on('frame', function() while true do end end)");
    const fs::path broken = write_script(folder, "broken.luau", "this is not luau");
    // The SDK's example, at the game's armor model lookup: model 42 for any part.
    store(runtime, 0x08869778u, {0x2402002Au, 0x03E00008u, 0u}); // addiu v0, zero, 42; jr ra
    // Its check made for this stand-in, so hooks by name take it for the game's.
    {
        std::vector<mhp3rd::mods::symbols::Symbol> table = mhp3rd::mods::symbols::all();
        std::array<std::uint8_t, 16> first{};
        runtime.memory().copy_out(0x08869778u, first);
        for (mhp3rd::mods::symbols::Symbol &symbol : table)
            if (symbol.name == "armor.model_of_part") symbol.check = mhp3rd::mods::symbols::fnv1a(first);
        mhp3rd::mods::symbols::use_table_for_tests(std::move(table));
    }
    const fs::path example = write_script(folder / "example", "main.luau",
#include "example_script.inc"
    );

    // The game's code that reads the second model file of each set.
    store(runtime, 0x08856668u, {0x3C020898u, 0x24422E64u});

    mhp3rd::mods::scripts::start(runtime,
        {{"script", main, {}}, {"loop", loop, {}}, {"broken", broken, {}}, {"gone", folder / "x.luau", {}},
            {"example", example, {}}});
    check(logged("script started"), "scripts: a script starts");
    check(logged("memory true true"), "scripts: memory reads and writes, checked");
    check(logged("ranges true true true"), "scripts: out-of-range values and NaN addresses are errors; -1 is all ones");
    check(logged("setting true 2"), "scripts: settings fall back, choices are 1-based");
    check(logged("setting now true 2 nil 4 4"), "scripts: a declared setting reads back as declared; others are nil");
    check(logged("bad key true nil"), "scripts: a setting key that could break the choices file is not declared");
    check(logged("sandbox true true true"),
        "scripts: no io, no os beyond clocks and dates, and the built-ins are read-only");
    check(logged("cannot compile the script"), "scripts: a syntax error is reported");
    check(logged("cannot read x.luau"), "scripts: a missing script is reported");

    check(call(runtime, 0x08A00000u, 10u) == 11u && logged("after v0=11"), "scripts: before and after hooks");
    check(call(runtime, 0x08A00100u, 0u) == 0x99u, "scripts: a replace hook returns its value");
    (void)call(runtime, 0x08A00000u, 10u);
    std::size_t one_shots = 0u;
    for (const auto &report : mhp3rd::mods::native::reports()) one_shots += report.line == "one-shot ran" ? 1u : 0u;
    check(one_shots == 1u && !logged("removed hook ran"),
        "scripts: a removed hook runs no more, also one that removes itself while it runs");
    (void)call(runtime, 0x08A00200u, 0u);
    check(logged("instruction t1=7"), "scripts: an instruction hook");
    std::this_thread::sleep_for(std::chrono::milliseconds(6)); // the last callback's time runs out
    check(mhp3rd::mods::scripts::out_of_time(-1) && !mhp3rd::mods::scripts::out_of_time(0) &&
            !mhp3rd::mods::scripts::out_of_time(2),
        "scripts: out of time stops the VM, never the collector, which also runs between callbacks");

    runtime.cpu().gpr[5] = 0u; // the hunter played here
    runtime.cpu().gpr[6] = 3u; // the legs
    check(call(runtime, 0x08869778u, 0u) == 42u && logged("part 3 uses model 42") && logged("logging armor models"),
        "scripts: the SDK's example script logs a model the lookup returns");

    check(logged("symbols true true game_task true armor.model_of_part true"),
        "scripts: the game's functions and data by name and by address");
    check(logged("unknown hook true true") && logged("no known function called no.such"),
        "scripts: a hook on an unknown name is refused and said");
    check(logged("monsters 58 15 true true true") &&
            logged("monster: the model file none.pac is not in the mod's folder") &&
            logged("monster: the picture file ../picture.png is not in the mod's folder"),
        "scripts: monsters by id, and one whose files are missing is not added, its picture's too");
    check(logged("tables true 74 2 true 80 true true true") && logged("table no.such is not extended: no such table"),
        "scripts: a game table where it is, made longer; an unknown one is nil");
    check(logged("overlay at true") && logged("bad event true"),
        "scripts: no overlay there; an unknown event is an error");

    // What the scripts draw, through stand-in widgets.
    const std::vector<mhp3rd::mods::native::Panel> panels = mhp3rd::mods::native::panels("script", false);
    const std::vector<mhp3rd::mods::native::Panel> overlays = mhp3rd::mods::native::panels({}, true);
    check(logged("panels true true") && panels.size() == 1u && overlays.size() == 1u &&
            mhp3rd::mods::native::panels("example", false).size() == 1u,
        "scripts: panels on the mod's page and an overlay over the game");
    panels[0].draw(&kWidgets, panels[0].user);
    overlays[0].draw(&kWidgets, overlays[0].user);
    check(logged("panel true true 4 true 0.25 true typed te true true true"),
        "scripts: a panel's widgets and what the player changed, an input within its capacity");
    check(logged("overlay drew") && g_shapes == 4, "scripts: an overlay's shapes");
    const std::vector<mhp3rd::mods::native::Panel> example_panels = mhp3rd::mods::native::panels("example", false);
    example_panels[0].draw(&kWidgets, example_panels[0].user);
    check(!logged("script error"), "scripts: the example's panel draws");

    // A quest between two village overlays.
    runtime.memory().store32(0x08AB3640u, 0x09000000u);
    runtime.memory().store16(0x09000000u + 0x60472u, 10503u);
    place_overlay(runtime, 0x0A05E600u, "lobby_task.ovl");
    mhp3rd::mods::native::overlay_loaded(runtime, 0x0A05E600u);
    place_overlay(runtime, 0x0A05E600u, "game_task.ovl");
    mhp3rd::mods::native::overlay_loaded(runtime, 0x0A05E600u);
    place_overlay(runtime, 0x0A05E600u, "lobby_task.ovl");
    mhp3rd::mods::native::overlay_loaded(runtime, 0x0A05E600u);
    check(logged("quest started 10503") && logged("quest ended 10503") && logged("overlay gone 168158720") &&
            logged("quest 10503 started") && logged("quest 10503 ended"),
        "scripts: quests starting and ending, and an overlay leaving its slot");

    mhp3rd::mods::native::frame(runtime);
    check(logged("widgets outside a draw refused true"), "scripts: widgets only exist while drawing");
    check(logged("started, frames so far 0"), "scripts: the start event before the first frame's");
    check(logged("took too long"), "scripts: a script that loops forever is stopped");
    check(logged("stale cpu refused true"), "scripts: the CPU cannot be used after its callback");
    check(mhp3rd::mods::scripts::running() == 2u, "scripts: only the working scripts keep running");

    // A script at its memory limit when its hook is called: the proxy for the
    // CPU cannot be made, and the script stops with an error of its own
    // instead of an exception through the game's code.
    store(runtime, 0x08A00400u, {0x24020003u, 0x03E00008u, 0u}); // addiu v0, zero, 3; jr ra
    const fs::path hungry = write_script(folder, "hungry.luau", R"(
yakumo.hook(0x08A00400, "before", function(cpu) cpu.a0 = 1 end)
yakumo.log("hungry hooked")
)");
    mhp3rd::mods::scripts::start(runtime, {{"hungry", hungry, {}}});
    mhp3rd::mods::scripts::limit_memory_for_tests(1u);
    bool threw = false;
    std::uint32_t result = 0u;
    try {
        result = call(runtime, 0x08A00400u, 0u);
    } catch (...) {
        threw = true;
    }
    mhp3rd::mods::scripts::limit_memory_for_tests(0u);
    check(logged("hungry hooked") && !threw && result == 3u && logged("not enough memory") &&
            mhp3rd::mods::scripts::running() == 2u,
        "scripts: a script out of memory when its hook is called stops with an error, and the game goes on");

    mhp3rd::mods::scripts::reset_for_tests();
    std::error_code ec;
    fs::remove_all(folder, ec);
    std::printf("%s\n", failures == 0 ? "all script mod checks passed" : "script mod checks FAILED");
    return failures == 0 ? 0 : 1;
}
