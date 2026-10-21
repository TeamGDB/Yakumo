#include "mods/mod_script_internal.hpp"

#include "mods/mhp3rd_mods.hpp"
#include "mods/mod_ini.hpp"

#include "luacode.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>

namespace mhp3rd::mods::scripts {

const ymod_widgets *g_widgets = nullptr;

namespace {

constexpr auto kCallBudget = std::chrono::milliseconds(5);
constexpr auto kStartBudget = std::chrono::milliseconds(250);
constexpr std::size_t kMemoryLimit = 64u * 1024u * 1024u;
constexpr int kCpuTag = 1;
constexpr const char *kCpuMeta = "yakumo.cpu";

std::vector<std::unique_ptr<Script>> &scripts() {
    static std::vector<std::unique_ptr<Script>> list;
    return list;
}
std::deque<Callback> &callbacks() {
    static std::deque<Callback> list; // stable addresses: hooks keep pointers
    return list;
}
// Callbacks of removed hooks, for the next ones.
std::vector<Callback *> &unused_callbacks() {
    static std::vector<Callback *> list;
    return list;
}
Clock::time_point g_deadline;

void *allocate(void *user, void *block, std::size_t old_size, std::size_t new_size) {
    auto *script = static_cast<Script *>(user);
    if (new_size == 0u) {
        std::free(block);
        script->used -= old_size;
        return nullptr;
    }
    if (script->used - old_size + new_size > kMemoryLimit) return nullptr;
    void *grown = std::realloc(block, new_size);
    if (grown != nullptr) script->used = script->used - old_size + new_size;
    return grown;
}

void interrupt(lua_State *state, int gc_state) {
    if (out_of_time(gc_state)) luaL_error(state, "the script took too long and was stopped");
}

// --- The guest CPU, while a callback runs ---

constexpr const char *kRegisterNames[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3",
    "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp",
    "ra"};

ymod_cpu *cpu_of(lua_State *state) {
    auto **box = static_cast<ymod_cpu **>(lua_touserdatatagged(state, 1, kCpuTag));
    if (box == nullptr || *box == nullptr) luaL_error(state, "the CPU is only there while its callback runs");
    return *box;
}

// Which field a name means: a register (0-31), hi (32), lo (33), pc (34),
// an FPU register (100-131), or -1.
int field(const char *name) {
    for (int r = 0; r < 32; ++r)
        if (std::strcmp(name, kRegisterNames[r]) == 0) return r;
    if (std::strcmp(name, "s8") == 0) return 30;
    if (std::strcmp(name, "hi") == 0) return 32;
    if (std::strcmp(name, "lo") == 0) return 33;
    if (std::strcmp(name, "pc") == 0) return 34;
    if ((name[0] == 'r' || name[0] == 'f') && name[1] >= '0' && name[1] <= '9') {
        char *end = nullptr;
        const long n = std::strtol(name + 1, &end, 10);
        if (*end == '\0' && n >= 0 && n < 32) return static_cast<int>(name[0] == 'r' ? n : 100 + n);
    }
    return -1;
}

int cpu_index(lua_State *state) {
    ymod_cpu *cpu = cpu_of(state);
    const int f = field(luaL_checkstring(state, 2));
    if (f < 0) luaL_error(state, "no register called %s", lua_tostring(state, 2));
    if (f >= 100)
        lua_pushnumber(state, cpu->fpr[f - 100]);
    else
        lua_pushnumber(state, f < 32 ? cpu->gpr[f] : f == 32 ? cpu->hi : f == 33 ? cpu->lo : cpu->pc);
    return 1;
}

int cpu_newindex(lua_State *state) {
    ymod_cpu *cpu = cpu_of(state);
    const int f = field(luaL_checkstring(state, 2));
    if (f < 0 || f == 0) luaL_error(state, "cannot set %s", lua_tostring(state, 2));
    if (f >= 100) {
        cpu->fpr[f - 100] = static_cast<float>(luaL_checknumber(state, 3));
        return 0;
    }
    const std::uint32_t value = word_argument(state, 3);
    if (f < 32)
        cpu->gpr[f] = value;
    else if (f == 32)
        cpu->hi = value;
    else if (f == 33)
        cpu->lo = value;
    else
        cpu->pc = value;
    return 0;
}

// Pushes a CPU proxy; returns the slot to clear after the call.
ymod_cpu **push_cpu(lua_State *state, ymod_cpu *cpu) {
    auto **box = static_cast<ymod_cpu **>(lua_newuserdatatagged(state, sizeof(ymod_cpu *), kCpuTag));
    *box = cpu;
    luaL_getmetatable(state, kCpuMeta);
    lua_setmetatable(state, -2);
    return box;
}

// The metatable of the cpu a hook callback gets.
void install_cpu(lua_State *state) {
    luaL_newmetatable(state, kCpuMeta);
    lua_pushcfunction(state, &cpu_index, "cpu.__index");
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, &cpu_newindex, "cpu.__newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pop(state, 1);
}

bool load(Script &script, const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        log(script, "cannot read " + to_utf8(path.filename()), YMOD_LOG_ERROR);
        return false;
    }
    const std::string source{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    std::size_t size = 0u;
    char *bytecode = luau_compile(source.data(), source.size(), &options, &size);
    const std::string chunk = "=" + to_utf8(path.filename());
    const int status = luau_load(script.thread, chunk.c_str(), bytecode, size, 0);
    std::free(bytecode);
    if (status != 0) {
        const char *message = lua_tostring(script.thread, -1);
        log(script, std::string("cannot compile the script: ") + (message != nullptr ? message : "?"), YMOD_LOG_ERROR);
        lua_pop(script.thread, 1);
        return false;
    }
    return call(script, 0, 0, kStartBudget);
}

} // namespace

// --- Shared with the yakumo.* functions (mod_script_internal.hpp) ---

void log(Script &script, const std::string &line, int level) {
    script.host->log(script.mod, level, line.c_str());
}

// Runs what is on the thread's stack; on an error the script stops being called.
bool call(Script &script, int arguments, int results, Clock::duration budget) {
    if (script.failed) {
        lua_pop(script.thread, arguments + 1);
        return false;
    }
    g_deadline = Clock::now() + budget;
    if (lua_pcall(script.thread, arguments, results, 0) == LUA_OK) return true;
    const char *message = lua_tostring(script.thread, -1);
    log(script, std::string("script error, it stops: ") + (message != nullptr ? message : "?"), YMOD_LOG_ERROR);
    lua_pop(script.thread, 1);
    script.failed = true;
    // Its hooks, events, panels and overlays go: none would call it again.
    native::withdraw(script.mod);
    return false;
}

Script &self(lua_State *state) {
    return *static_cast<Script *>(lua_touserdata(state, lua_upvalueindex(1)));
}

std::uint32_t address_argument(lua_State *state, int index) {
    const double value = luaL_checknumber(state, index);
    if (!(value >= 0.0 && value <= 4294967295.0)) luaL_error(state, "address out of range"); // NaN too
    return static_cast<std::uint32_t>(value);
}

// A value for a register or memory: a whole number from -2^31 to 2^32 - 1,
// negative ones as two's complement.
std::uint32_t word_argument(lua_State *state, int index) {
    const double value = luaL_checknumber(state, index);
    if (!(value >= -2147483648.0 && value <= 4294967295.0)) luaL_error(state, "value out of range");
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(value));
}

int hook_callback(ymod_cpu *cpu, void *user) {
    auto *callback = static_cast<Callback *>(user);
    if (callback->script == nullptr) return YMOD_CONTINUE; // removed
    Script &script = *callback->script;
    if (script.failed) return YMOD_CONTINUE;
    lua_getref(script.thread, callback->ref);
    ymod_cpu **box = push_cpu(script.thread, cpu);
    int result = YMOD_CONTINUE;
    if (call(script, 1, 1)) {
        if (lua_toboolean(script.thread, -1)) result = YMOD_RETURN;
        lua_pop(script.thread, 1);
    }
    *box = nullptr;
    return result;
}

void event_callback(int, std::uint32_t value, void *user) {
    auto *callback = static_cast<Callback *>(user);
    Script &script = *callback->script;
    if (script.failed) return;
    lua_getref(script.thread, callback->ref);
    lua_pushnumber(script.thread, value);
    (void)call(script, 1, 0);
}

void draw_callback(const ymod_widgets *ui, void *user) {
    auto *callback = static_cast<Callback *>(user);
    Script &script = *callback->script;
    if (script.failed || ui == nullptr) return;
    const ymod_widgets *outer = std::exchange(g_widgets, ui);
    lua_getref(script.thread, callback->ref);
    lua_getref(script.thread, script.widgets);
    (void)call(script, 1, 0);
    g_widgets = outer;
}

Callback *keep(lua_State *state, int index) {
    luaL_checktype(state, index, LUA_TFUNCTION);
    const Callback callback{&self(state), lua_ref(state, index)};
    if (!unused_callbacks().empty()) {
        Callback *reused = unused_callbacks().back();
        unused_callbacks().pop_back();
        *reused = callback;
        return reused;
    }
    callbacks().push_back(callback);
    return &callbacks().back();
}

// The function goes back to the script's collector; the slot to the next hook.
void release(Callback *callback) {
    if (callback == nullptr || callback->script == nullptr) return;
    lua_unref(callback->script->state, callback->ref);
    callback->script = nullptr;
    unused_callbacks().push_back(callback);
}

// A hook's number for the script, keeping its callback while it is in place.
void push_hook(lua_State *state, Script &script, ymod_hook id, Callback *callback) {
    if (id == 0u) {
        release(callback);
        lua_pushnil(state);
        return;
    }
    script.hooked[id] = callback;
    lua_pushnumber(state, id);
}

void start(psprecomp::Runtime &runtime, const std::vector<Resolution::Source> &sources) {
    if (sources.empty()) return;
    native::attach(runtime);
    ModSession *session = mods::session();
    for (const Resolution::Source &source : sources) {
        std::string name = source.mod;
        std::string folder;
        if (session != nullptr) {
            if (const Mod *found = session->library().find(source.mod)) {
                name = found->name;
                folder = to_utf8(found->folder);
            }
        }
        auto script = std::make_unique<Script>();
        Script &s = *script;
        s.mod = native::add_mod(source.mod, name, folder);
        s.host = native::host();
        s.memory = static_cast<const ymod_memory *>(s.host->service(s.mod, YMOD_MEMORY, 1u));
        s.hooks = static_cast<const ymod_hooks *>(s.host->service(s.mod, YMOD_HOOKS, 1u));
        s.events = static_cast<const ymod_events *>(s.host->service(s.mod, YMOD_EVENTS, 1u));
        s.files = static_cast<const ymod_files *>(s.host->service(s.mod, YMOD_FILES, 1u));
        s.settings = static_cast<const ymod_settings *>(s.host->service(s.mod, YMOD_SETTINGS, 1u));
        s.symbols = static_cast<const ymod_symbols *>(s.host->service(s.mod, YMOD_SYMBOLS, 1u));
        s.tables = static_cast<const ymod_tables *>(s.host->service(s.mod, YMOD_TABLES, 1u));
        s.monsters = static_cast<const ymod_monsters *>(s.host->service(s.mod, YMOD_MONSTERS, 1u));
        s.ui = static_cast<const ymod_ui *>(s.host->service(s.mod, YMOD_UI, 1u));
        s.state = lua_newstate(&allocate, &s);
        if (s.state == nullptr) {
            log(s, "cannot create the script's state", YMOD_LOG_ERROR);
            continue;
        }
        lua_callbacks(s.state)->interrupt = &interrupt;
        luaL_openlibs(s.state);
        install_cpu(s.state);
        install_api(s);
        // Built-ins and globals read-only; the script runs on its own thread
        // with a writable environment of its own.
        luaL_sandbox(s.state);
        s.thread = lua_newthread(s.state);
        luaL_sandboxthread(s.thread);
        if (load(s, source.path)) {
            log(s, "script started");
        } else {
            s.failed = true;
            native::withdraw(s.mod);
        }
        scripts().push_back(std::move(script));
    }
}

bool out_of_time(int gc_state) {
    return gc_state < 0 && Clock::now() > g_deadline;
}

std::size_t running() {
    std::size_t count = 0u;
    for (const auto &script : scripts())
        if (!script->failed) ++count;
    return count;
}

void reset_for_tests() {
    callbacks().clear();
    unused_callbacks().clear();
    for (auto &script : scripts())
        if (script->state != nullptr) lua_close(script->state);
    scripts().clear();
}

} // namespace mhp3rd::mods::scripts
