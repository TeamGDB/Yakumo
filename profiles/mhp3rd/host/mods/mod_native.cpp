#include "mods/mod_native_internal.hpp"

#include "mods/mhp3rd_mods.hpp"
#include "mods/mod_ini.hpp"

#include "kernel/kernel.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mhp3rd::mods::native {

State &state() {
    static State s;
    return s;
}

void report(ymod_mod *mod, const std::string &line, bool problem) {
    const std::string name = mod != nullptr ? mod->name : std::string("native mods");
    std::cout << "[mod " << name << "] " << line << "\n";
    std::vector<Report> &reports = state().reports;
    // A mod that logs every frame keeps only its latest lines: the oldest
    // line that is not a problem goes first, and a line that is not a problem
    // never takes a problem's place.
    constexpr std::size_t kKept = 1000u;
    if (reports.size() >= kKept) {
        const auto oldest = std::find_if(reports.begin(), reports.end(), [](const Report &r) { return !r.problem; });
        if (oldest == reports.end() && !problem) return;
        reports.erase(oldest != reports.end() ? oldest : reports.begin());
    }
    reports.push_back({name, line, problem});
}

namespace {

namespace fs = std::filesystem;

void *open_library(const fs::path &path, std::string &error) {
#if defined(_WIN32)
    HMODULE handle = LoadLibraryW(path.c_str());
    if (handle == nullptr) error = "Windows error " + std::to_string(GetLastError());
    return reinterpret_cast<void *>(handle);
#else
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char *text = dlerror();
        error = text != nullptr ? text : "cannot load";
    }
    return handle;
#endif
}

void *library_symbol(void *handle, const char *name) {
#if defined(_WIN32)
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

// --- The host table ---

const void *host_service(ymod_mod *, const char *name, std::uint32_t version) {
    if (name == nullptr) return nullptr;
    struct Entry {
        const char *name;
        std::uint32_t version;
        const void *table;
    };
    static const Entry kEntries[] = {
        {YMOD_MEMORY, 1u, &kMemory},
        {YMOD_HOOKS, 1u, &kHooks},
        {YMOD_CALLS, 1u, &kCalls},
        {YMOD_EVENTS, 1u, &kEvents},
        {YMOD_FILES, 1u, &kFiles},
        {YMOD_SETTINGS, 1u, &kSettings},
        {YMOD_SERVICES, 1u, &kServices},
        {YMOD_SYMBOLS, 1u, &kSymbols},
        {YMOD_UI, 1u, &kUi},
        {YMOD_TABLES, 1u, &kTables},
        {YMOD_MONSTERS, 1u, &kMonsters},
        {YMOD_QUESTS, 1u, &kQuests},
        {YMOD_EQUIPMENT, 1u, &kEquipment},
        {YMOD_MAPS, 1u, &kMaps},
    };
    for (const Entry &entry : kEntries)
        if (std::strcmp(entry.name, name) == 0) return version <= entry.version ? entry.table : nullptr;
    return nullptr;
}

void host_log(ymod_mod *mod, int level, const char *message) {
    report(mod, message != nullptr ? message : "", level >= YMOD_LOG_WARNING);
}

const char *host_folder(ymod_mod *mod) {
    return mod->folder.c_str();
}

const ymod_host kHost{YAKUMO_API_VERSION, sizeof(ymod_host), &host_service, &host_log, &host_folder};

} // namespace

const ymod_host *host() {
    return &kHost;
}

void attach(Runtime &runtime) {
    State &s = state();
    s.runtime = &runtime;
    // Where functions with AFTER hooks return to: a word of guest memory the
    // host owns.
    if (s.trampoline == 0u) {
        const std::int32_t uid = kernel().allocate_block("mod hook returns", 4u, 16u, 16u);
        if (const MemoryBlock *block = uid >= 0 ? kernel().find_block(uid) : nullptr) {
            s.trampoline = block->address;
            runtime.register_function(s.trampoline, &hook_return, "mod_hook_return");
        }
    }
    psprecomp::set_jit_instruction_hook(&instruction_dispatch);
}

ymod_mod *add_mod(const std::string &id, const std::string &name, const std::string &folder) {
    auto mod = std::make_unique<ymod_mod>();
    mod->id = id;
    mod->name = name;
    mod->folder = folder;
    ymod_mod *raw = mod.get();
    state().mods.push_back(std::move(mod));
    return raw;
}

void start(Runtime &runtime, const std::vector<Resolution::Source> &sources) {
    if (sources.empty()) return;
    attach(runtime);
    State &s = state();
    ModSession *session = mods::session();
    for (const Resolution::Source &source : sources) {
        auto mod = std::make_unique<ymod_mod>();
        mod->id = source.mod;
        mod->name = source.mod;
        if (session != nullptr) {
            if (const Mod *found = session->library().find(source.mod)) {
                mod->name = found->name;
                mod->folder = to_utf8(found->folder);
            }
        }
        ymod_mod *raw = mod.get();
        s.mods.push_back(std::move(mod));
        std::string error;
        raw->library = open_library(source.path, error);
        if (raw->library == nullptr) {
            report(raw, "cannot load " + to_utf8(source.path.filename()) + ": " + error, true);
            raw->failed = true;
            continue;
        }
        // A mod that says which interface it was built for is refused when
        // that is newer than this host's.
        if (const auto *version =
                static_cast<const std::uint32_t *>(library_symbol(raw->library, YAKUMO_MOD_API_VERSION_NAME));
            version != nullptr && *version > YAKUMO_API_VERSION) {
            report(raw,
                "needs a newer Yakumo (mod interface " + std::to_string(*version) + ", this Yakumo has " +
                    std::to_string(YAKUMO_API_VERSION) + ")",
                true);
            raw->failed = true;
            continue;
        }
        const auto main = reinterpret_cast<ymod_main_fn>(library_symbol(raw->library, YAKUMO_MOD_MAIN_NAME));
        if (main == nullptr) {
            report(raw, to_utf8(source.path.filename()) + " has no " YAKUMO_MOD_MAIN_NAME, true);
            raw->failed = true;
            continue;
        }
        const int result = main(&kHost, raw);
        if (result != 0) {
            report(raw, "refused to start (" + std::to_string(result) + ")", true);
            withdraw(raw);
            continue;
        }
        report(raw, "started", false);
    }
}

void frame(Runtime &runtime) {
    State &s = state();
    if (s.mods.empty()) return;
    s.runtime = &runtime;
    reconcile();
    if (!s.started) {
        s.started = true;
        fire(YMOD_EVENT_GAME_STARTED, 0u);
    }
    s.game.look(runtime.memory(), &fire);
    s.game.next_frame();
    fire(YMOD_EVENT_FRAME, 0u);
}

void file_loaded(FileId file) {
    if (state().mods.empty()) return;
    fire(YMOD_EVENT_FILE_LOADED, file);
}

void overlay_loaded(Runtime &runtime, std::uint32_t base) {
    State &s = state();
    if (s.mods.empty()) return;
    s.runtime = &runtime;
    reconcile();
    // The overlay this one replaced went first.
    s.game.look(runtime.memory(), &fire);
    if (const std::uint32_t loaded = s.game.overlay_loaded(runtime.memory(), base); loaded != 0u)
        fire(YMOD_EVENT_OVERLAY_LOADED, loaded);
}

std::vector<Panel> panels(const std::string &mod, bool overlays) {
    std::vector<Panel> out;
    for (const Drawing &drawing : state().drawings) {
        if (drawing.overlay != overlays || (!overlays && drawing.mod->id != mod)) continue;
        out.push_back({drawing.mod->name, drawing.title, drawing.draw, drawing.user});
    }
    return out;
}

const std::vector<Report> &reports() {
    return state().reports;
}

void withdraw(ymod_mod *mod) {
    if (mod == nullptr) return;
    mod->failed = true;
    State &s = state();
    std::vector<ymod_hook> hooks;
    for (const auto &[id, where] : s.hook_sites) {
        const std::vector<Hook> &list = where.second ? s.instructions[where.first].hooks : s.sites[where.first].hooks;
        for (const Hook &hook : list)
            if (hook.id == id && hook.mod == mod) hooks.push_back(id);
    }
    for (const ymod_hook hook : hooks) hooks_remove(mod, hook);
    std::erase_if(s.subscribers, [mod](const Subscriber &subscriber) { return subscriber.mod == mod; });
    std::erase_if(s.drawings, [mod](const Drawing &drawing) { return drawing.mod == mod; });
    std::erase_if(s.services, [mod](const auto &entry) { return entry.second.owner == mod; });
}

bool runs(const ymod_mod *mod) {
    return mod == nullptr || !mod->failed;
}

std::size_t running() {
    // Yakumo's own users of the API ("yakumo." ids, such as quests) are not mods.
    const std::vector<std::unique_ptr<ymod_mod>> &mods = state().mods;
    return static_cast<std::size_t>(std::count_if(
        mods.begin(), mods.end(), [](const auto &mod) { return !mod->failed && !mod->id.starts_with("yakumo."); }));
}

void reset_for_tests() {
    State &s = state();
    s = State{};
}

} // namespace mhp3rd::mods::native
