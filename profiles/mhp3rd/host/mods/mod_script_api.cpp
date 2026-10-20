#include "mods/mod_script_internal.hpp"

#include "mods/mod_library.hpp"

#include <cstring>
#include <iterator>
#include <string>

// The functions of the global table yakumo, and the widgets a panel's or an
// overlay's draw callback gets (docs/MODDING.md, "Script mods").
namespace mhp3rd::mods::scripts {
namespace {

constexpr std::uint32_t kMaxBytes = 1024u * 1024u; // readbytes

int api_log(lua_State *state) {
    std::string line;
    for (int i = 1, n = lua_gettop(state); i <= n; ++i) {
        std::size_t length = 0u;
        const char *text = luaL_tolstring(state, i, &length);
        if (i > 1) line += ' ';
        line.append(text, length);
        lua_pop(state, 1);
    }
    log(self(state), line);
    return 0;
}

template <typename T> int api_read(lua_State *state) {
    T value{};
    if (self(state).memory->read(address_argument(state, 1), &value, sizeof(T)) != 0) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushnumber(state, static_cast<double>(value));
    return 1;
}

template <typename T> int api_write(lua_State *state) {
    const auto value = static_cast<T>(word_argument(state, 2));
    lua_pushboolean(state, self(state).memory->write(address_argument(state, 1), &value, sizeof(T)) == 0);
    return 1;
}

int api_readfloat(lua_State *state) {
    float value{};
    if (self(state).memory->read(address_argument(state, 1), &value, 4u) != 0) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushnumber(state, value);
    return 1;
}

int api_writefloat(lua_State *state) {
    const auto value = static_cast<float>(luaL_checknumber(state, 2));
    lua_pushboolean(state, self(state).memory->write(address_argument(state, 1), &value, 4u) == 0);
    return 1;
}

int api_readbytes(lua_State *state) {
    const std::uint32_t address = address_argument(state, 1);
    const auto length = static_cast<std::uint32_t>(luaL_checkinteger(state, 2));
    if (length > kMaxBytes) luaL_error(state, "at most %u bytes at once", kMaxBytes);
    std::string bytes(length, '\0');
    if (length != 0u && self(state).memory->read(address, bytes.data(), length) != 0) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushlstring(state, bytes.data(), bytes.size());
    return 1;
}

int api_writebytes(lua_State *state) {
    std::size_t length = 0u;
    const char *bytes = luaL_checklstring(state, 2, &length);
    lua_pushboolean(state,
        length != 0u &&
            self(state).memory->write(address_argument(state, 1), bytes, static_cast<std::uint32_t>(length)) == 0);
    return 1;
}

int api_allocate(lua_State *state) {
    Script &script = self(state);
    const std::uint32_t address =
        script.memory->allocate(script.mod, static_cast<std::uint32_t>(luaL_checkinteger(state, 1)), 16u);
    if (address == 0u)
        lua_pushnil(state);
    else
        lua_pushnumber(state, address);
    return 1;
}

int api_code_changed(lua_State *state) {
    self(state).memory->code_changed(
        address_argument(state, 1), static_cast<std::uint32_t>(luaL_checkinteger(state, 2)));
    return 0;
}

int api_on(lua_State *state) {
    const char *name = luaL_checkstring(state, 1);
    struct Event {
        const char *name;
        int event;
    };
    static const Event kEvents[] = {
        {"frame", YMOD_EVENT_FRAME},
        {"file", YMOD_EVENT_FILE_LOADED},
        {"overlay", YMOD_EVENT_OVERLAY_LOADED},
        {"overlay_unloaded", YMOD_EVENT_OVERLAY_UNLOADED},
        {"start", YMOD_EVENT_GAME_STARTED},
        {"quest_started", YMOD_EVENT_QUEST_STARTED},
        {"quest_ended", YMOD_EVENT_QUEST_ENDED},
    };
    int event = -1;
    for (const Event &known : kEvents)
        if (std::strcmp(name, known.name) == 0) event = known.event;
    if (event < 0)
        luaL_error(state,
            "no event called %s (frame, file, overlay, overlay_unloaded, start, quest_started, quest_ended)", name);
    Script &script = self(state);
    script.events->subscribe(script.mod, event, &event_callback, keep(state, 2));
    return 0;
}

// A hook's target: an address, with the overlay as an optional argument, or
// the name of a known function. False when the name is not given out.
bool hook_target(lua_State *state, Script &script, int overlay_index, std::uint32_t &address, const char *&overlay) {
    overlay = luaL_optstring(state, overlay_index, nullptr);
    if (lua_type(state, 1) != LUA_TSTRING) {
        address = address_argument(state, 1);
        return true;
    }
    ymod_symbol found{sizeof(ymod_symbol)};
    if (script.symbols == nullptr || script.symbols->find(lua_tostring(state, 1), &found) == 0u ||
        found.kind != YMOD_SYMBOL_FUNCTION) {
        log(script, std::string("no known function called ") + lua_tostring(state, 1), YMOD_LOG_WARNING);
        return false;
    }
    address = found.address;
    overlay = found.overlay;
    return true;
}

int api_hook(lua_State *state) {
    Script &script = self(state);
    std::uint32_t address = 0u;
    const char *overlay = nullptr;
    if (!hook_target(state, script, 4, address, overlay)) {
        lua_pushnil(state);
        return 1;
    }
    const char *kind_name = luaL_checkstring(state, 2);
    const int kind = std::strcmp(kind_name, "before") == 0 ? YMOD_HOOK_BEFORE
        : std::strcmp(kind_name, "after") == 0             ? YMOD_HOOK_AFTER
        : std::strcmp(kind_name, "replace") == 0           ? YMOD_HOOK_REPLACE
                                                           : -1;
    if (kind < 0) luaL_error(state, "a hook is before, after or replace");
    Callback *callback = keep(state, 3);
    push_hook(
        state, script, script.hooks->function(script.mod, overlay, address, kind, &hook_callback, callback), callback);
    return 1;
}

int api_hook_instruction(lua_State *state) {
    Script &script = self(state);
    std::uint32_t address = 0u;
    const char *overlay = nullptr;
    if (!hook_target(state, script, 3, address, overlay)) {
        lua_pushnil(state);
        return 1;
    }
    Callback *callback = keep(state, 2);
    push_hook(
        state, script, script.hooks->instruction(script.mod, overlay, address, &hook_callback, callback), callback);
    return 1;
}

int api_unhook(lua_State *state) {
    Script &script = self(state);
    const auto id = static_cast<ymod_hook>(luaL_checkinteger(state, 1));
    script.hooks->remove(script.mod, id);
    if (const auto found = script.hooked.find(id); found != script.hooked.end()) {
        release(found->second);
        script.hooked.erase(found);
    }
    return 0;
}

// The host refuses a key that could break the choices file; so does the
// script's own record of what it declared.
void declared(Script &script, const char *key, char kind) {
    if (valid_setting_key(key)) script.setting_kinds[key] = kind;
}

int api_setting_switch(lua_State *state) {
    Script &script = self(state);
    declared(script, luaL_checkstring(state, 1), 's');
    lua_pushboolean(state,
        script.settings->declare_switch(
            script.mod, luaL_checkstring(state, 1), luaL_checkstring(state, 2), lua_toboolean(state, 3)) != 0);
    return 1;
}

int api_setting_number(lua_State *state) {
    Script &script = self(state);
    declared(script, luaL_checkstring(state, 1), 'n');
    lua_pushnumber(state,
        static_cast<double>(
            script.settings->declare_number(script.mod, luaL_checkstring(state, 1), luaL_checkstring(state, 2),
                luaL_checkinteger(state, 3), luaL_checkinteger(state, 4), luaL_checkinteger(state, 5))));
    return 1;
}

// Choices as a list {"Off", "Low", "High"}; the value is 1-based, as Luau's.
int api_setting_choice(lua_State *state) {
    Script &script = self(state);
    luaL_checktype(state, 3, LUA_TTABLE);
    std::string choices;
    for (int i = 1, n = lua_objlen(state, 3); i <= n; ++i) {
        lua_rawgeti(state, 3, i);
        if (i > 1) choices += ';';
        choices += luaL_checkstring(state, -1);
        lua_pop(state, 1);
    }
    const std::int64_t fallback = luaL_optinteger(state, 4, 1) - 1;
    declared(script, luaL_checkstring(state, 1), 'c');
    lua_pushnumber(state,
        static_cast<double>(script.settings->declare_choice(script.mod, luaL_checkstring(state, 1),
                                luaL_checkstring(state, 2), choices.c_str(), fallback) +
            1));
    return 1;
}

// A declared setting's value now, as its declaration returned it; nil for a
// setting the script never declared.
int api_setting(lua_State *state) {
    Script &script = self(state);
    const char *key = luaL_checkstring(state, 1);
    const auto kind = script.setting_kinds.find(key);
    if (kind == script.setting_kinds.end()) {
        lua_pushnil(state);
        return 1;
    }
    const std::int64_t value = script.settings->get(script.mod, key);
    if (kind->second == 's')
        lua_pushboolean(state, value != 0);
    else
        lua_pushnumber(state, static_cast<double>(kind->second == 'c' ? value + 1 : value));
    return 1;
}

int api_replace_file(lua_State *state) {
    Script &script = self(state);
    std::size_t length = 0u;
    const char *bytes = luaL_checklstring(state, 2, &length);
    lua_pushboolean(state,
        script.files->replace(script.mod, address_argument(state, 1), bytes, static_cast<std::uint32_t>(length)) == 0);
    return 1;
}

int api_original_file(lua_State *state) {
    Script &script = self(state);
    const std::uint32_t file = address_argument(state, 1);
    const std::int64_t size = script.files->read_original(file, nullptr, 0u);
    if (size < 0 || size > 64 * 1024 * 1024) {
        lua_pushnil(state);
        return 1;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    (void)script.files->read_original(file, bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    lua_pushlstring(state, bytes.data(), bytes.size());
    return 1;
}

// A known function's or data's address and overlay by name; nil when unknown.
int api_symbol(lua_State *state) {
    Script &script = self(state);
    ymod_symbol found{sizeof(ymod_symbol)};
    if (script.symbols == nullptr || script.symbols->find(luaL_checkstring(state, 1), &found) == 0u) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushnumber(state, found.address);
    if (found.overlay != nullptr)
        lua_pushstring(state, found.overlay);
    else
        lua_pushnil(state);
    return 2;
}

int api_symbol_name(lua_State *state) {
    Script &script = self(state);
    ymod_symbol found{sizeof(ymod_symbol)};
    if (script.symbols == nullptr || script.symbols->name_of(address_argument(state, 1), &found) != 0)
        lua_pushnil(state);
    else
        lua_pushstring(state, found.name);
    return 1;
}

// The overlay loaded now over an address: its name and load address, or nil.
// A game table's address now, its rows and its row size; nil when unknown.
int api_table(lua_State *state) {
    Script &script = self(state);
    ymod_table found{sizeof(ymod_table)};
    if (script.tables == nullptr || script.tables->find(luaL_checkstring(state, 1), &found) != 0) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushnumber(state, found.address);
    lua_pushnumber(state, found.rows);
    lua_pushnumber(state, found.row);
    return 3;
}

// Adds a large monster from a table {id, based_on, model, geometry, motions,
// effects (optional)}:
// its kind's number, or nil (the reason is in the log).
int api_add_monster(lua_State *state) {
    Script &script = self(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const auto field = [state](const char *name) -> std::string {
        lua_getfield(state, 1, name);
        std::string value = lua_isstring(state, -1) ? lua_tostring(state, -1) : "";
        lua_pop(state, 1);
        return value;
    };
    const std::string id = field("id"), base = field("based_on"), model = field("model"), geometry = field("geometry"),
                      motions = field("motions"), effects = field("effects");
    const ymod_monster monster{sizeof(ymod_monster), id.c_str(), base.c_str(), model.c_str(), geometry.c_str(),
        motions.c_str(), effects.empty() ? nullptr : effects.c_str()};
    const int32_t kind = script.monsters != nullptr ? script.monsters->add(script.mod, &monster) : -1;
    if (kind < 0)
        lua_pushnil(state);
    else
        lua_pushnumber(state, kind);
    return 1;
}

// A large monster's kind by id (the game's or a mod's), or nil.
int api_monster_kind(lua_State *state) {
    Script &script = self(state);
    const int32_t kind = script.monsters != nullptr ? script.monsters->kind(luaL_checkstring(state, 1)) : -1;
    if (kind < 0)
        lua_pushnil(state);
    else
        lua_pushnumber(state, kind);
    return 1;
}

// Gives a table at least that many rows: where they are, or nil.
int api_extend_table(lua_State *state) {
    Script &script = self(state);
    const double rows = luaL_checknumber(state, 2);
    const std::uint32_t address = script.tables != nullptr && rows >= 0.0 && rows <= 65536.0
        ? script.tables->extend(script.mod, luaL_checkstring(state, 1), static_cast<std::uint32_t>(rows))
        : 0u;
    if (address == 0u)
        lua_pushnil(state);
    else
        lua_pushnumber(state, address);
    return 1;
}

int api_overlay_at(lua_State *state) {
    Script &script = self(state);
    char name[64]{};
    const std::uint32_t base =
        script.symbols != nullptr ? script.symbols->overlay_at(address_argument(state, 1), name, sizeof(name)) : 0u;
    if (base == 0u) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushstring(state, name);
    lua_pushnumber(state, base);
    return 2;
}

int add_drawing(lua_State *state, bool overlay) {
    Script &script = self(state);
    const char *title = luaL_checkstring(state, 1);
    Callback *callback = keep(state, 2);
    const int result = script.ui == nullptr ? -1
        : overlay                           ? script.ui->overlay(script.mod, title, &draw_callback, callback)
                                            : script.ui->panel(script.mod, title, &draw_callback, callback);
    lua_pushboolean(state, result == 0);
    return 1;
}

int api_panel(lua_State *state) {
    return add_drawing(state, false);
}

int api_overlay(lua_State *state) {
    return add_drawing(state, true);
}

int api_folder(lua_State *state) {
    Script &script = self(state);
    lua_pushstring(state, script.host->folder(script.mod));
    return 1;
}

// --- The widgets a draw callback gets ---

const ymod_widgets &widgets(lua_State *state) {
    if (g_widgets == nullptr) luaL_error(state, "widgets are only there while a panel or an overlay draws");
    return *g_widgets;
}

std::uint32_t colour_argument(lua_State *state, int index) {
    if (lua_isnoneornil(state, index)) return 0xFFFFFFFFu;
    return address_argument(state, index);
}

int ui_text(lua_State *state) {
    const ymod_widgets &ui = widgets(state);
    const char *text = luaL_checkstring(state, 1);
    if (lua_isnoneornil(state, 2))
        ui.text(text);
    else
        ui.text_colored(colour_argument(state, 2), text);
    return 0;
}

int ui_separator(lua_State *state) {
    widgets(state).separator();
    return 0;
}

int ui_same_line(lua_State *state) {
    widgets(state).same_line();
    return 0;
}

int ui_section(lua_State *state) {
    lua_pushboolean(state, widgets(state).section(luaL_checkstring(state, 1)) != 0);
    return 1;
}

int ui_button(lua_State *state) {
    lua_pushboolean(state, widgets(state).button(luaL_checkstring(state, 1)) != 0);
    return 1;
}

int ui_checkbox(lua_State *state) {
    int value = lua_toboolean(state, 2);
    const bool changed = widgets(state).checkbox(luaL_checkstring(state, 1), &value) != 0;
    lua_pushboolean(state, value);
    lua_pushboolean(state, changed);
    return 2;
}

int ui_slider(lua_State *state) {
    auto value = static_cast<std::int32_t>(luaL_checkinteger(state, 2));
    const bool changed = widgets(state).slider_int(luaL_checkstring(state, 1), &value,
                             static_cast<std::int32_t>(luaL_checkinteger(state, 3)),
                             static_cast<std::int32_t>(luaL_checkinteger(state, 4))) != 0;
    lua_pushnumber(state, value);
    lua_pushboolean(state, changed);
    return 2;
}

int ui_slider_float(lua_State *state) {
    auto value = static_cast<float>(luaL_checknumber(state, 2));
    const bool changed =
        widgets(state).slider_float(luaL_checkstring(state, 1), &value, static_cast<float>(luaL_checknumber(state, 3)),
            static_cast<float>(luaL_checknumber(state, 4))) != 0;
    lua_pushnumber(state, value);
    lua_pushboolean(state, changed);
    return 2;
}

int ui_input(lua_State *state) {
    std::size_t length = 0u;
    const char *text = luaL_checklstring(state, 2, &length);
    const auto capacity = static_cast<std::size_t>(luaL_optinteger(state, 3, 256));
    if (capacity < 1u || capacity > 4096u) luaL_error(state, "an input holds 1 to 4096 bytes");
    std::string buffer(capacity + 1u, '\0');
    std::memcpy(buffer.data(), text, std::min(length, capacity));
    const bool changed = widgets(state).input_text(
                             luaL_checkstring(state, 1), buffer.data(), static_cast<std::uint32_t>(capacity + 1u)) != 0;
    lua_pushstring(state, buffer.c_str());
    lua_pushboolean(state, changed);
    return 2;
}

float coordinate(lua_State *state, int index) {
    return static_cast<float>(luaL_checknumber(state, index));
}

int ui_line(lua_State *state) {
    widgets(state).line(coordinate(state, 1), coordinate(state, 2), coordinate(state, 3), coordinate(state, 4),
        colour_argument(state, 5), static_cast<float>(luaL_optnumber(state, 6, 1.0)));
    return 0;
}

int ui_rect(lua_State *state) {
    widgets(state).rect(coordinate(state, 1), coordinate(state, 2), coordinate(state, 3), coordinate(state, 4),
        colour_argument(state, 5), lua_toboolean(state, 6));
    return 0;
}

int ui_circle(lua_State *state) {
    widgets(state).circle(coordinate(state, 1), coordinate(state, 2), coordinate(state, 3), colour_argument(state, 4),
        lua_toboolean(state, 5));
    return 0;
}

int ui_text_at(lua_State *state) {
    widgets(state).text_at(
        coordinate(state, 1), coordinate(state, 2), colour_argument(state, 4), luaL_checkstring(state, 3));
    return 0;
}

} // namespace

void install_api(Script &script) {
    lua_State *state = script.state;
    struct Function {
        const char *name;
        lua_CFunction fn;
    };
    static const Function kFunctions[] = {
        {"log", &api_log},
        {"read8", &api_read<std::uint8_t>},
        {"read16", &api_read<std::uint16_t>},
        {"read32", &api_read<std::uint32_t>},
        {"readfloat", &api_readfloat},
        {"readbytes", &api_readbytes},
        {"write8", &api_write<std::uint8_t>},
        {"write16", &api_write<std::uint16_t>},
        {"write32", &api_write<std::uint32_t>},
        {"writefloat", &api_writefloat},
        {"writebytes", &api_writebytes},
        {"allocate", &api_allocate},
        {"code_changed", &api_code_changed},
        {"on", &api_on},
        {"hook", &api_hook},
        {"hook_instruction", &api_hook_instruction},
        {"unhook", &api_unhook},
        {"setting_switch", &api_setting_switch},
        {"setting_number", &api_setting_number},
        {"setting_choice", &api_setting_choice},
        {"setting", &api_setting},
        {"replace_file", &api_replace_file},
        {"original_file", &api_original_file},
        {"folder", &api_folder},
        {"symbol", &api_symbol},
        {"symbol_name", &api_symbol_name},
        {"overlay_at", &api_overlay_at},
        {"table", &api_table},
        {"extend_table", &api_extend_table},
        {"add_monster", &api_add_monster},
        {"monster_kind", &api_monster_kind},
        {"panel", &api_panel},
        {"overlay", &api_overlay},
    };
    lua_createtable(state, 0, static_cast<int>(std::size(kFunctions)) + 1);
    for (const Function &function : kFunctions) {
        lua_pushlightuserdata(state, &script);
        lua_pushcclosure(state, function.fn, function.name, 1);
        lua_setfield(state, -2, function.name);
    }
    lua_pushnumber(state, YAKUMO_API_VERSION);
    lua_setfield(state, -2, "api_version");
    lua_setglobal(state, "yakumo");

    static const Function kWidgets[] = {
        {"text", &ui_text},
        {"separator", &ui_separator},
        {"same_line", &ui_same_line},
        {"section", &ui_section},
        {"button", &ui_button},
        {"checkbox", &ui_checkbox},
        {"slider", &ui_slider},
        {"slider_float", &ui_slider_float},
        {"input", &ui_input},
        {"line", &ui_line},
        {"rect", &ui_rect},
        {"circle", &ui_circle},
        {"text_at", &ui_text_at},
    };
    lua_createtable(state, 0, static_cast<int>(std::size(kWidgets)));
    for (const Function &function : kWidgets) {
        lua_pushcfunction(state, function.fn, function.name);
        lua_setfield(state, -2, function.name);
    }
    lua_setreadonly(state, -1, true);
    script.widgets = lua_ref(state, -1);
    lua_pop(state, 1);
}

} // namespace mhp3rd::mods::scripts
