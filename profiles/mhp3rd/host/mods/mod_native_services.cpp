#include "mods/mod_native_internal.hpp"

#include "mods/mhp3rd_code_mods.hpp"
#include "mods/mhp3rd_mod_format.hpp"
#include "mods/mhp3rd_mods.hpp"
#include "mods/mhp3rd_monsters.hpp"
#include "mods/mhp3rd_quests.hpp"
#include "mods/mhp3rd_symbols.hpp"
#include "mods/mhp3rd_tables.hpp"
#include "mods/mod_ini.hpp"

#include "game/quest_builder.hpp"
#include "install/user_data.hpp"
#include "kernel/kernel.hpp"
#include "overlays.hpp"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

// The services besides hooks: guest memory, events, files, settings,
// services between mods, the game's symbols and drawing in the interface.
namespace mhp3rd::mods::native {
namespace {

namespace fs = std::filesystem;

int memory_read(std::uint32_t address, void *out, std::uint32_t length) {
    Runtime *runtime = state().runtime;
    if (runtime == nullptr || length == 0u || !runtime->memory().contains(address, length)) return -1;
    runtime->memory().copy_out(address, {static_cast<std::uint8_t *>(out), length});
    return 0;
}

int memory_write(std::uint32_t address, const void *data, std::uint32_t length) {
    Runtime *runtime = state().runtime;
    if (runtime == nullptr || length == 0u || !runtime->memory().contains(address, length)) return -1;
    runtime->memory().copy_in(address, {static_cast<const std::uint8_t *>(data), length});
    return 0;
}

std::uint32_t memory_allocate(ymod_mod *mod, std::uint32_t size, std::uint32_t alignment) {
    if (size == 0u || alignment < 16u || (alignment & (alignment - 1u)) != 0u) return 0u;
    const std::int32_t uid = kernel().allocate_block("mod " + mod->id, 4u, size, alignment);
    if (uid < 0) return 0u;
    const MemoryBlock *block = kernel().find_block(uid);
    if (block == nullptr) return 0u;
    mod->allocations.push_back(static_cast<std::uint32_t>(uid));
    return block->address;
}

void memory_release(ymod_mod *mod, std::uint32_t address) {
    for (auto it = mod->allocations.begin(); it != mod->allocations.end(); ++it) {
        const MemoryBlock *block = kernel().find_block(static_cast<std::int32_t>(*it));
        if (block == nullptr || block->address != address) continue;
        (void)kernel().free_block(static_cast<std::int32_t>(*it));
        mod->allocations.erase(it);
        return;
    }
}

void memory_code_changed(std::uint32_t address, std::uint32_t length) {
    Runtime *runtime = state().runtime;
    if (runtime == nullptr) return;
    std::vector<std::string> problems;
    (void)p3rd::retire_code(*runtime, address, length, "a native mod", problems);
    for (const std::string &line : problems) report(nullptr, line, true);
}

int events_subscribe(ymod_mod *mod, int event, ymod_event_fn callback, void *user) {
    if (callback == nullptr || event < YMOD_EVENT_FRAME || event > YMOD_EVENT_QUEST_ENDED || mod->failed) return -1;
    state().subscribers.push_back({mod, event, callback, user});
    return 0;
}

void events_unsubscribe(ymod_mod *mod, int event, ymod_event_fn callback, void *user) {
    std::erase_if(state().subscribers, [&](const Subscriber &subscriber) {
        return subscriber.mod == mod && subscriber.event == event && subscriber.callback == callback &&
            subscriber.user == user;
    });
}

int files_replace(ymod_mod *mod, std::uint32_t file, const void *data, std::uint32_t length) {
    ModSession *session = mods::session();
    if (session == nullptr || data == nullptr || length == 0u) return -1;
    fs::path folder;
    try {
        folder = install::user_data_directory() / "mods-cache" / from_utf8(mod->id);
    } catch (const std::exception &) {
        return -1;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path path = folder / from_utf8(session->library().format().file_name(file) + ".bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char *>(data), static_cast<std::streamsize>(length));
        if (!out) return -1;
    }
    session->library().add_runtime_change(mod->id, FileChange{FileChange::Kind::Replace, file, path, {}});
    session->commit();
    return 0;
}

std::int64_t files_read_original(std::uint32_t file, void *out, std::uint32_t capacity) {
    const std::optional<std::vector<std::uint8_t>> bytes = mods::original_file(file);
    if (!bytes) return -1;
    if (out != nullptr) std::memcpy(out, bytes->data(), std::min<std::size_t>(capacity, bytes->size()));
    return static_cast<std::int64_t>(bytes->size());
}

std::int64_t declare(ymod_mod *mod, ModSetting setting) {
    ModSession *session = mods::session();
    if (!valid_setting_key(setting.key)) {
        report(mod,
            "a setting key must be 1 to 64 letters, digits, '_', '.' or '-': \"" + setting.key.substr(0, 64) +
                "\" refused",
            true);
        return setting.fallback;
    }
    mod->fallbacks[setting.key] = setting.fallback;
    if (session == nullptr) return setting.fallback;
    return session->library().declare_setting(mod->id, setting);
}

std::int64_t settings_switch(ymod_mod *mod, const char *key, const char *label, int fallback) {
    return declare(mod,
        ModSetting{key != nullptr ? key : "", label != nullptr ? label : "", ModSetting::Kind::Switch, 0, 1,
            fallback != 0 ? 1 : 0, {}});
}

std::int64_t settings_number(ymod_mod *mod, const char *key, const char *label, std::int64_t minimum,
    std::int64_t maximum, std::int64_t fallback) {
    return declare(mod,
        ModSetting{key != nullptr ? key : "", label != nullptr ? label : "", ModSetting::Kind::Number, minimum, maximum,
            fallback, {}});
}

std::int64_t settings_choice(
    ymod_mod *mod, const char *key, const char *label, const char *choices, std::int64_t fallback) {
    return declare(mod,
        ModSetting{key != nullptr ? key : "", label != nullptr ? label : "", ModSetting::Kind::Choice, 0, 0, fallback,
            split_list(choices != nullptr ? choices : "")});
}

std::int64_t settings_get(ymod_mod *mod, const char *key) {
    if (key == nullptr) return 0;
    // Until the player changes it, a setting has its declared fallback.
    const auto declared = mod->fallbacks.find(key);
    const std::int64_t fallback = declared != mod->fallbacks.end() ? declared->second : 0;
    ModSession *session = mods::session();
    return session != nullptr ? session->library().setting(mod->id, key, fallback) : fallback;
}

int services_publish(ymod_mod *mod, const char *name, std::uint32_t version, const void *table) {
    if (name == nullptr || table == nullptr || mod->failed) return -1;
    std::map<std::string, Service> &services = state().services;
    if (services.contains(name)) return -1;
    services[name] = {mod, version, table};
    return 0;
}

const void *services_import(ymod_mod *, const char *name, std::uint32_t version) {
    if (name == nullptr) return nullptr;
    const auto found = state().services.find(name);
    if (found == state().services.end() || found->second.version < version) return nullptr;
    return found->second.table;
}

// Fills in as much of `out` as the mod's struct holds, by its size.
void give(const symbols::Symbol &symbol, ymod_symbol *out) {
    if (out == nullptr || out->size < sizeof(std::uint32_t)) return;
    const ymod_symbol entry{sizeof(ymod_symbol), symbol.name.c_str(),
        symbol.overlay.empty() ? nullptr : symbol.overlay.c_str(), symbol.about.c_str(), symbol.address,
        symbol.function ? static_cast<std::uint32_t>(YMOD_SYMBOL_FUNCTION)
                        : static_cast<std::uint32_t>(YMOD_SYMBOL_DATA)};
    const std::uint32_t size = out->size;
    const std::size_t fits = std::min<std::size_t>(size, sizeof(ymod_symbol));
    std::memcpy(static_cast<void *>(out), &entry, fits);
    out->size = size;
}

std::uint32_t symbols_find(const char *name, ymod_symbol *out) {
    if (name == nullptr) return 0u;
    Runtime *runtime = state().runtime;
    const symbols::Symbol *symbol = symbols::find(name, runtime != nullptr ? &runtime->memory() : nullptr);
    if (symbol == nullptr) return 0u;
    give(*symbol, out);
    return symbol->address;
}

int symbols_name_of(std::uint32_t address, ymod_symbol *out) {
    const symbols::Symbol *symbol = symbols::at(address);
    if (symbol == nullptr) return -1;
    give(*symbol, out);
    return 0;
}

int symbols_get(std::uint32_t index, ymod_symbol *out) {
    const std::vector<symbols::Symbol> &table = symbols::all();
    if (index >= table.size()) return -1;
    give(table[index], out);
    return 0;
}

std::uint32_t symbols_overlay_at(std::uint32_t address, char *out, std::uint32_t capacity) {
    Runtime *runtime = state().runtime;
    if (runtime == nullptr) return 0u;
    const std::optional<LoadedOverlay> loaded = overlay_at(runtime->memory(), address);
    if (!loaded) return 0u;
    if (out != nullptr && capacity != 0u) {
        const std::size_t length = std::min<std::size_t>(loaded->name.size(), capacity - 1u);
        std::memcpy(out, loaded->name.data(), length);
        out[length] = '\0';
    }
    return loaded->base;
}

int add_drawing(ymod_mod *mod, const char *title, ymod_draw_fn draw, void *user, bool overlay) {
    if (draw == nullptr || mod->failed) return -1;
    state().drawings.push_back({mod, title != nullptr ? title : "", draw, user, overlay});
    return 0;
}

int ui_panel(ymod_mod *mod, const char *title, ymod_draw_fn draw, void *user) {
    return add_drawing(mod, title, draw, user, false);
}

int ui_overlay(ymod_mod *mod, const char *title, ymod_draw_fn draw, void *user) {
    return add_drawing(mod, title, draw, user, true);
}

void ui_remove(ymod_mod *mod, ymod_draw_fn draw, void *user) {
    std::erase_if(state().drawings,
        [&](const Drawing &drawing) { return drawing.mod == mod && drawing.draw == draw && drawing.user == user; });
}

// Fills in as much of `out` as the mod's struct holds, by its size.
void give_table(const tables::Table &table, ymod_table *out) {
    if (out == nullptr || out->size < sizeof(std::uint32_t)) return;
    const std::optional<tables::Placement> now = tables::placement(table.name);
    const ymod_table entry{sizeof(ymod_table), table.name.c_str(),
        table.overlay.empty() ? nullptr : table.overlay.c_str(), now ? now->address : table.address, table.row,
        now ? now->rows : table.rows, table.rows};
    const std::uint32_t size = out->size;
    std::memcpy(static_cast<void *>(out), &entry, std::min<std::size_t>(size, sizeof(ymod_table)));
    out->size = size;
}

int tables_find(const char *name, ymod_table *out) {
    const tables::Table *table = name != nullptr ? tables::find(name) : nullptr;
    if (table == nullptr) return -1;
    give_table(*table, out);
    return 0;
}

int tables_get(std::uint32_t index, ymod_table *out) {
    if (index >= tables::all().size()) return -1;
    give_table(tables::all()[index], out);
    return 0;
}

std::uint32_t tables_extend(ymod_mod *mod, const char *name, std::uint32_t rows) {
    Runtime *runtime = state().runtime;
    if (runtime == nullptr || name == nullptr || mod->failed) return 0u;
    std::string why;
    const std::optional<std::uint32_t> address = tables::extend(*runtime, name, rows, &why);
    if (!address) report(mod, std::string("table ") + name + " is not extended: " + why, true);
    return address.value_or(0u);
}

// --- Monsters ---

std::optional<std::vector<std::uint8_t>> mod_bytes(ymod_mod *mod, const char *relative, const char *what) {
    if (relative == nullptr || *relative == '\0') {
        report(mod, std::string("monster: no ") + what + " file given", true);
        return std::nullopt;
    }
    const std::optional<std::filesystem::path> path = p3rd::mod_file(mod->folder, relative);
    if (!path) {
        report(mod, std::string("monster: the ") + what + " file " + relative + " is not in the mod's folder", true);
        return std::nullopt;
    }
    std::ifstream in(*path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) report(mod, std::string("monster: the ") + what + " file " + relative + " is empty", true);
    return bytes.empty() ? std::nullopt : std::optional(std::move(bytes));
}

int32_t monsters_add(ymod_mod *mod, const ymod_monster *monster) {
    Runtime *runtime = state().runtime;
    if (runtime == nullptr || mod == nullptr || mod->failed || monster == nullptr ||
        monster->size < offsetof(ymod_monster, motions) + sizeof(monster->motions))
        return -1;
    const auto model = mod_bytes(mod, monster->model, "model");
    const auto geometry = mod_bytes(mod, monster->geometry, "geometry");
    const auto motions = mod_bytes(mod, monster->motions, "motions");
    if (!model || !geometry || !motions) return -1;
    const bool has_effects = monster->size >= offsetof(ymod_monster, effects) + sizeof(monster->effects);
    monsters::Spec spec{monster->id != nullptr ? monster->id : "",
        monster->based_on != nullptr ? monster->based_on : "", *model, *geometry, *motions, mod->name, mod,
        has_effects && monster->effects != nullptr ? monster->effects : ""};
    std::string why;
    const std::optional<std::uint8_t> kind = monsters::add(*runtime, spec, &why);
    if (!kind) report(mod, "monster " + spec.id + " is not added: " + why, true);
    return kind ? int32_t{*kind} : -1;
}

int32_t monsters_kind(const char *id) {
    const std::optional<std::uint8_t> kind = id != nullptr ? monsters::kind(id) : std::nullopt;
    return kind ? int32_t{*kind} : -1;
}

int monsters_behave(ymod_mod *mod, int32_t kind, int part, ymod_monster_fn fn, void *user) {
    Runtime *runtime = state().runtime;
    if (runtime == nullptr || mod == nullptr || mod->failed) return -1;
    std::string why = "no such kind";
    if (kind >= 0 && kind <= 0xFF &&
        monsters::behave(*runtime, mod, static_cast<std::uint8_t>(kind), part, fn, user, &why))
        return 0;
    report(mod, "monster " + std::to_string(kind) + " is not given that behaviour: " + why, true);
    return -1;
}

int monsters_play_motion(uint32_t monster, int32_t motion, int32_t blend) {
    if (motion < 0 || motion > 0xFFFF || blend < 0) return -1;
    return monsters::play_motion(monster, static_cast<std::uint16_t>(motion), static_cast<std::uint32_t>(blend)) ? 0
                                                                                                                 : -1;
}

int monsters_motion_finished(uint32_t monster) {
    Runtime *runtime = state().runtime;
    const std::optional<bool> finished =
        runtime != nullptr ? monsters::motion_finished(*runtime, monster) : std::nullopt;
    return finished ? (*finished ? 1 : 0) : -1;
}

int monsters_at_frame(uint32_t monster, float frame) {
    Runtime *runtime = state().runtime;
    const std::optional<bool> passes = runtime != nullptr ? monsters::at_frame(*runtime, monster, frame) : std::nullopt;
    return passes ? (*passes ? 1 : 0) : -1;
}

int put_vec3(const std::optional<monsters::Vec3> &value, ymod_vec3 *out) {
    if (!value || out == nullptr) return -1;
    *out = {value->x, value->y, value->z};
    return 0;
}

int monsters_position(uint32_t monster, ymod_vec3 *out) {
    Runtime *runtime = state().runtime;
    return put_vec3(runtime != nullptr ? monsters::position(*runtime, monster) : std::nullopt, out);
}

int monsters_facing(uint32_t monster, float *degrees) {
    Runtime *runtime = state().runtime;
    const std::optional<float> value = runtime != nullptr ? monsters::facing(*runtime, monster) : std::nullopt;
    if (!value || degrees == nullptr) return -1;
    *degrees = *value;
    return 0;
}

int monsters_health(uint32_t monster, int32_t *now, int32_t *most) {
    Runtime *runtime = state().runtime;
    const std::optional<monsters::Health> value =
        runtime != nullptr ? monsters::health(*runtime, monster) : std::nullopt;
    if (!value) return -1;
    if (now != nullptr) *now = value->now;
    if (most != nullptr) *most = value->most;
    return 0;
}

int monsters_set_facing(uint32_t monster, float degrees) {
    Runtime *runtime = state().runtime;
    return runtime != nullptr && monsters::set_facing(*runtime, monster, degrees) ? 0 : -1;
}

int monsters_target(uint32_t monster, ymod_vec3 *out) {
    Runtime *runtime = state().runtime;
    return put_vec3(runtime != nullptr ? monsters::target(*runtime, monster) : std::nullopt, out);
}

int monsters_attack_shape(ymod_mod *mod, int32_t kind, int32_t shape, const ymod_hit_sphere *spheres, int32_t count) {
    if (mod == nullptr || mod->failed) return -1;
    std::vector<monsters::HitSphere> list;
    if (spheres != nullptr && count > 0 && count <= 16)
        for (int32_t i = 0; i < count; ++i)
            list.push_back({spheres[i].joint < 0 ? 0x100u : static_cast<std::uint32_t>(spheres[i].joint),
                spheres[i].radius, spheres[i].x, spheres[i].y, spheres[i].z});
    std::string why = "no such kind";
    if (kind >= 0 && kind <= 0xFF && shape >= 0 &&
        monsters::attack_shape(mod, static_cast<std::uint8_t>(kind), static_cast<std::uint32_t>(shape), list, &why))
        return 0;
    report(mod, "monster " + std::to_string(kind) + " is not given attack shape " + std::to_string(shape) + ": " + why,
        true);
    return -1;
}

int monsters_effect(uint32_t monster, int32_t effect, int32_t joint, const ymod_vec3 *offset, float scale) {
    if (effect < 0 || joint < 0 || joint > 0xFF || !(scale > 0.0f) || !std::isfinite(scale)) return -1;
    const monsters::Vec3 at = offset != nullptr ? monsters::Vec3{offset->x, offset->y, offset->z} : monsters::Vec3{};
    return monsters::effect(monster, static_cast<std::uint32_t>(effect), static_cast<std::uint32_t>(joint), at, scale)
        ? 0
        : -1;
}

int monsters_aim(uint32_t monster) {
    return monsters::aim(monster) ? 0 : -1;
}

int monsters_turn_to_target(uint32_t monster, float degrees) {
    Runtime *runtime = state().runtime;
    const std::optional<bool> faces =
        runtime != nullptr ? monsters::turn_to_target(*runtime, monster, degrees) : std::nullopt;
    return faces ? (*faces ? 1 : 0) : -1;
}

int monsters_move(uint32_t monster, float x, float z) {
    Runtime *runtime = state().runtime;
    return runtime != nullptr && monsters::move(*runtime, monster, x, z) ? 0 : -1;
}

int monsters_motion_frame(uint32_t monster, float *frame) {
    Runtime *runtime = state().runtime;
    const std::optional<float> value = runtime != nullptr ? monsters::motion_frame(*runtime, monster) : std::nullopt;
    if (!value || frame == nullptr) return -1;
    *frame = *value;
    return 0;
}

int monsters_attack(uint32_t monster, int32_t slot, int32_t attack) {
    if (slot < 0 || slot > 0xFF || attack < 0) return -1;
    return monsters::attack(monster, static_cast<std::uint8_t>(slot), static_cast<std::uint32_t>(attack)) ? 0 : -1;
}

int monsters_end_attack(uint32_t monster, int32_t slot) {
    if (slot < 0 || slot > 0xFF) return -1;
    return monsters::end_attack(monster, static_cast<std::uint8_t>(slot)) ? 0 : -1;
}

int32_t quests_add(ymod_mod *mod, const ymod_quest *quest) {
    Runtime *runtime = state().runtime;
    if (runtime == nullptr || mod == nullptr || mod->failed || quest == nullptr ||
        quest->size < offsetof(ymod_quest, texts) + sizeof(quest->texts))
        return -1;
    const auto text = [](const char *s) { return std::string(s != nullptr ? s : ""); };
    const auto kind_of = [](const char *id) { return id != nullptr ? monsters::kind(id) : std::nullopt; };
    const auto refuse = [mod, quest](const std::string &why) {
        report(mod, "quest " + std::to_string(quest->id) + " is not added: " + why, true);
        return -1;
    };
    game::QuestSpec spec;
    if (quest->id > 0xFFFFu || quest->map < 0 || quest->map > 0xFF || quest->stars < 0)
        return refuse("a number is out of range");
    spec.id = static_cast<std::uint16_t>(quest->id);
    spec.map = static_cast<std::uint8_t>(quest->map);
    spec.stars = static_cast<std::uint16_t>(quest->stars);
    spec.minutes = quest->minutes;
    spec.fee = quest->fee;
    spec.reward = quest->reward;
    const std::optional<std::uint8_t> target = kind_of(quest->target);
    if (!target) return refuse("its target \"" + text(quest->target) + "\" is not a monster's id");
    spec.target = *target;
    spec.count = static_cast<std::uint16_t>(std::clamp<std::uint32_t>(quest->count, 1u, 0xFFFFu));
    for (std::uint32_t i = 0; quest->monsters != nullptr && i < quest->monster_count && i < 6u; ++i) {
        const ymod_quest_monster &m = quest->monsters[i];
        const std::optional<std::uint8_t> kind = kind_of(m.monster);
        if (!kind) return refuse("its monster \"" + text(m.monster) + "\" is not a monster's id");
        if (m.area < 0 || m.area > 0xFF || m.health < 0 || m.health > 0xFF || m.attack < 0 || m.attack > 0xFF ||
            m.size < 0 || m.size > 0xFFFF)
            return refuse("a monster's area, levels or size are out of range");
        spec.monsters.push_back(
            {*kind, static_cast<std::uint16_t>(m.area), m.at.x, m.at.y, m.at.z, static_cast<std::uint8_t>(m.health),
                static_cast<std::uint8_t>(m.attack), static_cast<std::uint16_t>(m.size == 0 ? 100 : m.size)});
    }
    const ymod_quest_texts &t = quest->texts;
    spec.texts = {
        text(t.name), text(t.objective), text(t.failure), text(t.description), text(t.monsters), text(t.client)};
    std::string why;
    const std::optional<std::vector<std::uint8_t>> payload = game::build_quest(spec, &why);
    if (!payload) return refuse(why);
    if (!mhp3rd::mods::quests::add(*runtime, *payload, mod->name, &why)) return refuse(why);
    return static_cast<int32_t>(quest->id);
}

const char *monsters_id_of(int32_t kind) {
    if (kind < 0 || kind > 0xFF) return nullptr;
    const std::string_view id = monsters::id_of(static_cast<std::uint8_t>(kind));
    return id.empty() ? nullptr : id.data(); // ids are kept for the run, ending in a NUL
}

} // namespace

const ymod_tables kTables{1u, sizeof(ymod_tables), &tables_find, &tables_get, &tables_extend};

const ymod_quests kQuests{1u, sizeof(ymod_quests), &quests_add};

const ymod_monsters kMonsters{1u, sizeof(ymod_monsters), &monsters_add, &monsters_kind, &monsters_id_of,
    &monsters_behave, &monsters_play_motion, &monsters_motion_finished, &monsters_motion_frame, &monsters_at_frame,
    &monsters_position, &monsters_facing, &monsters_set_facing, &monsters_target, &monsters_aim,
    &monsters_turn_to_target, &monsters_move, &monsters_attack, &monsters_end_attack, &monsters_attack_shape,
    &monsters_effect, &monsters_health};

const ymod_memory kMemory{
    1u, sizeof(ymod_memory), &memory_read, &memory_write, &memory_allocate, &memory_release, &memory_code_changed};

const ymod_events kEvents{1u, sizeof(ymod_events), &events_subscribe, &events_unsubscribe};

void fire(int event, std::uint32_t value) {
    // A callback may subscribe more: iterate over a copy.
    const std::vector<Subscriber> subscribers = state().subscribers;
    for (const Subscriber &s : subscribers)
        if (s.event == event) s.callback(event, value, s.user);
}

const ymod_files kFiles{1u, sizeof(ymod_files), &files_replace, &files_read_original};

const ymod_settings kSettings{
    1u, sizeof(ymod_settings), &settings_switch, &settings_number, &settings_choice, &settings_get};

const ymod_services kServices{1u, sizeof(ymod_services), &services_publish, &services_import};

const ymod_symbols kSymbols{
    1u, sizeof(ymod_symbols), &symbols_find, &symbols_name_of, &symbols_get, &symbols_overlay_at};

const ymod_ui kUi{1u, sizeof(ymod_ui), &ui_panel, &ui_overlay, &ui_remove};

} // namespace mhp3rd::mods::native
