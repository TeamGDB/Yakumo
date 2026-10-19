// An example native mod: while its setting is on, it logs each armor model
// the played hunter's parts use, once per model, and shows the latest model
// of each part on the mod's page. It shows the parts of the API most mods
// need: hooks before and after a game function found by name, a setting and
// a panel on the mod's page, events, and the log. docs/MODDING.md describes
// the API.
#include "yakumo/mod.hpp"

#include <cstdint>
#include <set>
#include <string>

namespace {

// The game's armor model lookup, by its name in sdk/symbols.txt
// (docs/LAYERED_ARMOR.md): a1 is a hunter record's index, 0 for the hunter
// played here, a2 the part (0 to 6), and v0 the model it returns.
constexpr const char *kModelOfPart = "armor.model_of_part";
constexpr const char *kParts[] = {"Chest", "Arms", "Waist", "Legs", "Head", "Hair", "Face"};

yakumo::Mod *g_mod = nullptr;
bool g_logging = true;
std::uint32_t g_record = 0u;
std::uint32_t g_part = 0u;
std::set<std::uint32_t> g_seen; // part << 16 | model
std::uint32_t g_latest[7] = {};
bool g_known[7] = {};
unsigned g_files = 0u;

// Before the lookup runs: what it was asked for. Its arguments are gone by
// the time it returns.
int before_lookup(ymod_cpu *cpu, void *) {
    g_record = cpu->gpr[5]; // a1
    g_part = cpu->gpr[6];   // a2
    return YMOD_CONTINUE;
}

// After it returns: the model, in v0.
int after_lookup(ymod_cpu *cpu, void *) {
    const std::uint32_t model = cpu->gpr[2];
    if (g_record != 0u) return YMOD_CONTINUE;
    if (g_part < 7u) {
        g_latest[g_part] = model;
        g_known[g_part] = true;
    }
    if (g_logging && g_seen.insert(g_part << 16u | (model & 0xFFFFu)).second)
        g_mod->log("part " + std::to_string(g_part) + " uses model " + std::to_string(model));
    return YMOD_CONTINUE;
}

// On the mod's page, under its settings.
void models_panel(const ymod_widgets *ui, void *) {
    for (std::uint32_t part = 0u; part < 7u; ++part) {
        const std::string line = std::string(kParts[part]) + ": " +
            (g_known[part] ? std::to_string(g_latest[part]) : std::string("not seen yet"));
        ui->text(line.c_str());
    }
}

void on_event(int event, std::uint32_t value, void *) {
    switch (event) {
    case YMOD_EVENT_FRAME:
        // The player may change the setting on the mod's page at any time.
        g_logging = g_mod->settings()->get(g_mod->handle(), "log") != 0;
        break;
    case YMOD_EVENT_FILE_LOADED:
        ++g_files;
        break;
    case YMOD_EVENT_GAME_STARTED:
        g_mod->log("the game started after reading " + std::to_string(g_files) + " files");
        break;
    case YMOD_EVENT_QUEST_STARTED:
        g_mod->log("quest " + std::to_string(value) + " started");
        break;
    case YMOD_EVENT_QUEST_ENDED:
        g_mod->log("quest " + std::to_string(value) + " ended");
        break;
    default:
        break;
    }
}

} // namespace

YAKUMO_MOD_API_VERSION();

extern "C" YAKUMO_MOD_EXPORT int yakumo_mod_main(const ymod_host *host, ymod_mod *handle) {
    static yakumo::Mod mod(host, handle);
    g_mod = &mod;
    const ymod_hooks *hooks = mod.hooks();
    const ymod_events *events = mod.events();
    const ymod_settings *settings = mod.settings();
    // The services this mod cannot work without: refuse to run without them.
    if (hooks == nullptr || events == nullptr || settings == nullptr) return 1;

    g_logging = settings->declare_switch(handle, "log", "Log Armor Models", 1) != 0;
    // By name: a Yakumo that does not know the function, or a game whose code
    // there differs, gives no address, and the mod says so instead of hooking
    // the wrong code.
    if (mod.hook(kModelOfPart, YMOD_HOOK_BEFORE, &before_lookup) == 0u ||
        mod.hook(kModelOfPart, YMOD_HOOK_AFTER, &after_lookup) == 0u) {
        mod.log("cannot hook the armor model lookup", YMOD_LOG_ERROR);
        return 2;
    }
    for (const int event : {YMOD_EVENT_FRAME, YMOD_EVENT_FILE_LOADED, YMOD_EVENT_GAME_STARTED, YMOD_EVENT_QUEST_STARTED,
             YMOD_EVENT_QUEST_ENDED})
        events->subscribe(handle, event, &on_event, nullptr);
    // The panel is optional: a Yakumo older than the panels has no ui service.
    if (const ymod_ui *ui = mod.ui()) ui->panel(handle, "Models Now", &models_panel, nullptr);
    mod.log(g_logging ? "logging armor models" : "armor model logging is off");
    return 0;
}
