// A native mod for mhp3rd_native_mods_tests: it uses every service and logs
// what it sees, and the test checks the log.
#include "yakumo/mod.hpp"

#include <cstdio>
#include <optional>
#include <string>

namespace {

yakumo::Mod *g_mod = nullptr;
unsigned g_frames = 0u;

std::string format(const char *text, unsigned value) {
    char line[96];
    std::snprintf(line, sizeof(line), text, value);
    return line;
}

int before(ymod_cpu *cpu, void *) {
    cpu->gpr[4] += 1u; // a0, as the game's function sees it
    g_mod->log(format("before a0=%u", cpu->gpr[4]));
    return YMOD_CONTINUE;
}

int after(ymod_cpu *cpu, void *) {
    g_mod->log(format("after v0=%u", cpu->gpr[2]));
    return YMOD_CONTINUE;
}

int replace(ymod_cpu *cpu, void *) {
    cpu->gpr[2] = 0x99u;
    return YMOD_RETURN;
}

int instruction(ymod_cpu *cpu, void *) {
    g_mod->log(format("instruction t1=%u", cpu->gpr[9]));
    return YMOD_CONTINUE;
}

void event(int kind, std::uint32_t value, void *) {
    if (kind == YMOD_EVENT_OVERLAY_LOADED) g_mod->log(format("overlay loaded %08X", value));
    if (kind == YMOD_EVENT_OVERLAY_UNLOADED) g_mod->log(format("overlay unloaded %08X", value));
    if (kind == YMOD_EVENT_QUEST_STARTED) g_mod->log(format("quest started %u", value));
    if (kind == YMOD_EVENT_QUEST_ENDED) g_mod->log(format("quest ended %u", value));
    if (kind == YMOD_EVENT_GAME_STARTED) {
        g_mod->log("game started");
        g_mod->log(format("get=%u", static_cast<unsigned>(g_mod->settings()->get(g_mod->handle(), "on"))));
    }
    if (kind == YMOD_EVENT_FRAME) ++g_frames;
    if (kind == YMOD_EVENT_FILE_LOADED) g_mod->log("file event after unsubscribe");
}

// A panel with a switch, and an overlay with a line of text and a shape.
int g_switch = 0;
void panel(const ymod_widgets *ui, void *) {
    ui->text("a panel");
    if (ui->checkbox("Switch", &g_switch) != 0) g_mod->log(format("panel switch=%u", static_cast<unsigned>(g_switch)));
}
void overlay(const ymod_widgets *ui, void *) {
    ui->text("an overlay");
    ui->rect(0.1f, 0.1f, 0.2f, 0.2f, 0xFF0000FFu, 1);
    g_mod->log("overlay drawn");
}

unsigned frames() {
    return g_frames;
}

struct Counter {
    unsigned (*frames)();
};
const Counter kCounter{&frames};

} // namespace

#if defined(MHP3RD_FUTURE_API_TEST_MOD)
// Built as a stand-in for a mod made for a later interface: never started.
extern "C" {
YAKUMO_MOD_EXPORT extern const uint32_t yakumo_mod_api_version;
YAKUMO_MOD_EXPORT const uint32_t yakumo_mod_api_version = YAKUMO_API_VERSION + 1u;
}
#else
YAKUMO_MOD_API_VERSION();
#endif

extern "C" YAKUMO_MOD_EXPORT int yakumo_mod_main(const ymod_host *host, ymod_mod *handle) {
    static yakumo::Mod mod(host, handle);
    g_mod = &mod;
    const ymod_memory *memory = mod.memory();
    const ymod_hooks *hooks = mod.hooks();
    if (memory == nullptr || hooks == nullptr || mod.events() == nullptr) return 1;

    const std::uint32_t block = memory->allocate(handle, 64u, 16u);
    const bool stored = block != 0u && mod.write<std::uint32_t>(block, 0xC0DE1234u);
    const auto loaded = mod.read<std::uint32_t>(block);
    mod.log(stored && loaded && *loaded == 0xC0DE1234u ? "memory ok" : "memory failed");
    mod.log(mod.read<std::uint32_t>(0x00000010u) ? "unchecked read" : "bad address refused");

    mod.log(format("setting=%u", static_cast<unsigned>(mod.settings()->declare_switch(handle, "on", "On", 1))));

    if (hooks->function(handle, nullptr, 0x08A00000u, YMOD_HOOK_BEFORE, &before, nullptr) == 0u) return 2;
    if (hooks->function(handle, nullptr, 0x08A00000u, YMOD_HOOK_AFTER, &after, nullptr) == 0u) return 3;
    if (hooks->function(handle, nullptr, 0x08A00100u, YMOD_HOOK_REPLACE, &replace, nullptr) == 0u) return 4;
    if (hooks->instruction(handle, nullptr, 0x08A00204u, &instruction, nullptr) == 0u) return 5;
    if (hooks->function(handle, nullptr, 0x08A00002u, YMOD_HOOK_BEFORE, &before, nullptr) != 0u) return 6; // unaligned
    for (const int kind : {YMOD_EVENT_GAME_STARTED, YMOD_EVENT_FRAME, YMOD_EVENT_OVERLAY_LOADED,
             YMOD_EVENT_OVERLAY_UNLOADED, YMOD_EVENT_QUEST_STARTED, YMOD_EVENT_QUEST_ENDED})
        mod.events()->subscribe(handle, kind, &event, nullptr);
    if (mod.events()->subscribe(handle, 99, &event, nullptr) == 0) return 7;
    // Subscribed and taken back: never called.
    mod.events()->subscribe(handle, YMOD_EVENT_FILE_LOADED, &event, &g_frames);
    mod.events()->unsubscribe(handle, YMOD_EVENT_FILE_LOADED, &event, &g_frames);

    // The game's functions by name.
    const ymod_symbols *symbols = mod.symbols();
    if (symbols == nullptr) return 8;
    const std::optional<ymod_symbol> lookup = mod.symbol("armor.model_of_part");
    if (lookup && lookup->kind == YMOD_SYMBOL_FUNCTION && lookup->overlay == nullptr)
        mod.log(format("symbol armor.model_of_part %08X", lookup->address));
    mod.log(symbols->find("no.such.symbol", nullptr) == 0u ? "unknown symbol absent" : "unknown symbol found");
    ymod_symbol named{sizeof(ymod_symbol)};
    if (symbols->name_of(0x0A17370Cu, &named) == 0 && std::string(named.name) == "hud.quest" &&
        std::string(named.overlay) == "game_task")
        mod.log("symbol by address");
    unsigned count = 0u;
    while (symbols->get(count, &named) == 0) ++count;
    mod.log(count > 20u ? "symbols listed" : "symbols missing");

    const ymod_ui *ui = mod.ui();
    if (ui == nullptr || ui->panel(handle, "Test Panel", &panel, nullptr) != 0 ||
        ui->overlay(handle, "Test Overlay", &overlay, nullptr) != 0 || ui->panel(handle, "None", nullptr, nullptr) == 0)
        return 9;
    mod.services()->publish(handle, "test.counter", 1u, &kCounter);
    mod.log(host->service(handle, "yakumo.nothing", 1u) == nullptr ? "unknown service absent" : "unknown service");
    return 0;
}
