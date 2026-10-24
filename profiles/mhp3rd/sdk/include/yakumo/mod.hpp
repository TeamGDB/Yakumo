// A thin C++ layer over yakumo/mod.h: the same services, with typed lookups
// and lambdas for callbacks. Header-only; it adds no ABI of its own.
#ifndef YAKUMO_MOD_HPP
#define YAKUMO_MOD_HPP

#include "yakumo/mod.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>

namespace yakumo {

class Mod {
public:
    Mod(const ymod_host *host, ymod_mod *mod) noexcept : host_(host), mod_(mod) {}

    [[nodiscard]] ymod_mod *handle() const noexcept { return mod_; }
    [[nodiscard]] const ymod_host *host() const noexcept { return host_; }

    // A service table, or null when the host has none at that version.
    template <typename Table> const Table *service(const char *name, std::uint32_t version) const {
        return static_cast<const Table *>(host_->service(mod_, name, version));
    }
    const ymod_memory *memory() const { return service<ymod_memory>(YMOD_MEMORY, 1u); }
    const ymod_hooks *hooks() const { return service<ymod_hooks>(YMOD_HOOKS, 1u); }
    const ymod_calls *calls() const { return service<ymod_calls>(YMOD_CALLS, 1u); }
    const ymod_events *events() const { return service<ymod_events>(YMOD_EVENTS, 1u); }
    const ymod_files *files() const { return service<ymod_files>(YMOD_FILES, 1u); }
    const ymod_settings *settings() const { return service<ymod_settings>(YMOD_SETTINGS, 1u); }
    const ymod_services *services() const { return service<ymod_services>(YMOD_SERVICES, 1u); }
    const ymod_symbols *symbols() const { return service<ymod_symbols>(YMOD_SYMBOLS, 1u); }
    const ymod_ui *ui() const { return service<ymod_ui>(YMOD_UI, 1u); }
    const ymod_tables *tables() const { return service<ymod_tables>(YMOD_TABLES, 1u); }
    const ymod_monsters *monsters() const { return service<ymod_monsters>(YMOD_MONSTERS, 1u); }
    const ymod_quests *quests() const { return service<ymod_quests>(YMOD_QUESTS, 1u); }
    const ymod_equipment *equipment() const { return service<ymod_equipment>(YMOD_EQUIPMENT, 1u); }
    const ymod_maps *maps() const { return service<ymod_maps>(YMOD_MAPS, 1u); }

    // A known function or data by name (sdk/symbols.txt), or nothing.
    [[nodiscard]] std::optional<ymod_symbol> symbol(const char *name) const {
        const ymod_symbols *table = symbols();
        ymod_symbol found{};
        if (table == nullptr || table->find(name, &found) == 0u) return std::nullopt;
        return found;
    }
    // A function hook on a known function, by name: 0 when the name is
    // unknown or the hook fails.
    ymod_hook hook(const char *name, int kind, ymod_hook_fn callback, void *user = nullptr) const {
        const std::optional<ymod_symbol> found = symbol(name);
        const ymod_hooks *table = hooks();
        if (!found || found->kind != YMOD_SYMBOL_FUNCTION || table == nullptr) return 0u;
        return table->function(mod_, found->overlay, found->address, kind, callback, user);
    }

    // The monsters service's functions that take an array, from a C array or
    // a pointer and a count: they pass the array's stride (yakumo/mod.h,
    // "Sizes and strides"). 0, or -1 (the reason is in the log; -1 too when
    // the host has no such function).
    int attack_shape(std::int32_t kind, std::int32_t shape, const ymod_hit_sphere *spheres, std::size_t count) const {
        const ymod_monsters *table = monsters_having(offsetof(ymod_monsters, attack_shape), count);
        if (table == nullptr) return -1;
        const auto n = static_cast<std::int32_t>(count);
        return table->attack_shape(mod_, kind, shape, spheres, n, sizeof(ymod_hit_sphere));
    }
    template <std::size_t N>
    int attack_shape(std::int32_t kind, std::int32_t shape, const ymod_hit_sphere (&spheres)[N]) const {
        return attack_shape(kind, shape, spheres, N);
    }
    int body_shapes(std::int32_t kind, const ymod_body_capsule *capsules, std::size_t count) const {
        const ymod_monsters *table = monsters_having(offsetof(ymod_monsters, body_shapes), count);
        if (table == nullptr) return -1;
        return table->body_shapes(mod_, kind, capsules, static_cast<std::int32_t>(count), sizeof(ymod_body_capsule));
    }
    template <std::size_t N> int body_shapes(std::int32_t kind, const ymod_body_capsule (&capsules)[N]) const {
        return body_shapes(kind, capsules, N);
    }
    int footsteps(std::int32_t kind, const ymod_footstep *steps, std::size_t count) const {
        const ymod_monsters *table = monsters_having(offsetof(ymod_monsters, footsteps), count);
        if (table == nullptr) return -1;
        return table->footsteps(mod_, kind, steps, static_cast<std::int32_t>(count), sizeof(ymod_footstep));
    }
    template <std::size_t N> int footsteps(std::int32_t kind, const ymod_footstep (&steps)[N]) const {
        return footsteps(kind, steps, N);
    }

    void log(const std::string &message, int level = YMOD_LOG_INFO) const { host_->log(mod_, level, message.c_str()); }
    [[nodiscard]] std::string folder() const { return host_->folder(mod_); }

    // Guest memory as a value of type T, little-endian as on the PSP.
    template <typename T> std::optional<T> read(std::uint32_t address) const {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        const ymod_memory *table = memory();
        if (table == nullptr || table->read(address, &value, sizeof(T)) != 0) return std::nullopt;
        return value;
    }
    template <typename T> bool write(std::uint32_t address, const T &value) const {
        static_assert(std::is_trivially_copyable_v<T>);
        const ymod_memory *table = memory();
        return table != nullptr && table->write(address, &value, sizeof(T)) == 0;
    }

private:
    // The monsters service when it has the function at `field` and `count`
    // fits its argument, or null.
    [[nodiscard]] const ymod_monsters *monsters_having(std::size_t field, std::size_t count) const {
        const ymod_monsters *table = monsters();
        if (table == nullptr || table->size < field + sizeof(void *) ||
            count > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return nullptr;
        return table;
    }

    const ymod_host *host_;
    ymod_mod *mod_;
};

// One monster, as a part of behaviour sees it: the monsters service's
// functions on it, in the world's units and degrees (yakumo/mod.h). A
// function the host's service is too old for does nothing and answers no.
class Monster {
public:
    Monster(const ymod_monsters &service, std::uint32_t object) noexcept : service_(service), object_(object) {}

    [[nodiscard]] std::uint32_t object() const noexcept { return object_; }

    bool play(int motion, int blend = 0) const { return service_.play_motion(object_, motion, blend) == 0; }
    [[nodiscard]] bool finished() const { return service_.motion_finished(object_) == 1; }
    [[nodiscard]] float frame() const {
        float out = 0.0f;
        if (has(offsetof(ymod_monsters, motion_frame))) service_.motion_frame(object_, &out);
        return out;
    }
    [[nodiscard]] bool at_frame(float frame) const {
        return has(offsetof(ymod_monsters, at_frame)) && service_.at_frame(object_, frame) == 1;
    }

    [[nodiscard]] ymod_vec3 position() const {
        ymod_vec3 out{};
        if (has(offsetof(ymod_monsters, position))) service_.position(object_, &out);
        return out;
    }
    [[nodiscard]] float facing() const {
        float out = 0.0f;
        if (has(offsetof(ymod_monsters, facing))) service_.facing(object_, &out);
        return out;
    }
    bool set_facing(float degrees) const {
        return has(offsetof(ymod_monsters, set_facing)) && service_.set_facing(object_, degrees) == 0;
    }
    // Its health now (0 once it has fallen) and at most.
    [[nodiscard]] int health() const {
        int32_t now = 0;
        if (has(offsetof(ymod_monsters, health))) service_.health(object_, &now, nullptr);
        return now;
    }
    [[nodiscard]] int most_health() const {
        int32_t most = 0;
        if (has(offsetof(ymod_monsters, health))) service_.health(object_, nullptr, &most);
        return most;
    }
    [[nodiscard]] bool fallen() const { return has(offsetof(ymod_monsters, health)) && health() <= 0; }
    [[nodiscard]] ymod_vec3 target() const {
        ymod_vec3 out{};
        if (has(offsetof(ymod_monsters, target))) service_.target(object_, &out);
        return out;
    }
    // Has the game bring the target up to date (it lands for the next frame).
    bool aim() const { return has(offsetof(ymod_monsters, aim)) && service_.aim(object_) == 0; }
    // How far the target is, on the ground.
    [[nodiscard]] float distance_to_target() const {
        const ymod_vec3 at = position(), to = target();
        return std::hypot(to.x - at.x, to.z - at.z);
    }
    // Where the target is from the monster's facing, in degrees from -180 to
    // 180: turning by this much faces it.
    [[nodiscard]] float angle_to_target() const {
        const ymod_vec3 at = position(), to = target();
        const float towards = std::atan2(to.x - at.x, to.z - at.z) * 180.0f / 3.14159265f;
        float angle = std::fmod(towards - facing(), 360.0f);
        if (angle > 180.0f) angle -= 360.0f;
        if (angle <= -180.0f) angle += 360.0f;
        return angle;
    }
    // Turns towards the target by at most `degrees`; whether it faces it now.
    bool turn_to_target(float degrees) const {
        return has(offsetof(ymod_monsters, turn_to_target)) && service_.turn_to_target(object_, degrees) == 1;
    }
    // Moves it by (x, z) in its own frame: z ahead, x to the side.
    bool move(float x, float z) const { return has(offsetof(ymod_monsters, move)) && service_.move(object_, x, z) == 0; }
    bool move_forward(float distance) const { return move(0.0f, distance); }

    bool attack(int slot, int attack) const {
        return has(offsetof(ymod_monsters, attack)) && service_.attack(object_, slot, attack) == 0;
    }
    bool effect(int number, int joint, ymod_vec3 offset = {}, float scale = 1.0f) const {
        return has(offsetof(ymod_monsters, effect)) && service_.effect(object_, number, joint, &offset, scale) == 0;
    }
    bool end_attack(int slot) const {
        return has(offsetof(ymod_monsters, end_attack)) && service_.end_attack(object_, slot) == 0;
    }

private:
    [[nodiscard]] bool has(std::size_t field) const noexcept { return service_.size >= field + sizeof(void *); }

    const ymod_monsters &service_;
    std::uint32_t object_;
};

} // namespace yakumo

#endif // YAKUMO_MOD_HPP
