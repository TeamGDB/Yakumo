#include "game/damage_hook.hpp"
#include "game/damage_numbers.hpp"
#include "game/control_context.hpp"
#include "game/guest_ram.hpp"
#include "settings/settings.hpp"
#include "psprecomp/runtime.hpp"
#include <string>
#include <cstdlib>
#include <iostream>
namespace mhp3rd::game::combat {
namespace {
psprecomp::Runtime *owner{};
psprecomp::RegisteredFunction original{};
std::string original_name;
bool installed{};
DamageHistory history;
bool tracing() {
    static const bool enabled = std::getenv("MHP3RD_TRACE_DAMAGE") != nullptr;
    return enabled;
}
void apply_damage(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &context) {
    const auto function = original;
    if (!function || owner != &runtime) return;
    const auto generation = psprecomp::runtime_thread_switch_generation();
    const auto return_pc = context.gpr[31];
    const auto hit = settings::current().damage_numbers
        ? begin_hit(GuestRam(runtime.memory()),
              {context.gpr[4], context.gpr[20], context.gpr[18], return_pc, context.gpr[5] & 0xFFu})
        : std::nullopt;
    function(runtime, context);
    if (hit && !runtime.stopped() && context.pc == return_pc &&
        psprecomp::runtime_thread_switch_generation_matches(generation)) {
        const int amount = finish_hit(GuestRam(runtime.memory()), *hit);
        history.add(*hit, amount, damage_clock_us());
        static unsigned reported{};
        if (tracing() && amount > 0 && reported++ < 200u)
            std::cout << "[damage] own hit amount=" << amount << std::endl;
    }
}
}
void observe_damage_registration(
    psprecomp::Runtime &runtime, std::uint32_t address, psprecomp::RegisteredFunction function, std::string_view name) {
    if (address != kApplyDamage) return;
    owner = &runtime;
    original = function;
    original_name = name;
    installed = false;
    history.clear();
}
void damage_frame(psprecomp::Runtime &runtime) {
    const auto context = control_context::current();
    const bool desired = settings::current().damage_numbers && context.scene == control_context::Scene::Quest &&
        !context.interface_open && damage_code_supported(GuestRam(runtime.memory()));
    if (!desired)
        history.clear();
    else
        history.expire(damage_clock_us());
    if (owner != &runtime || !original || !damage_code_supported(GuestRam(runtime.memory()))) {
        installed = false;
        return;
    }
    if (desired && !installed) {
        runtime.register_function(kApplyDamage, &apply_damage, "host local damage observer");
        installed = true;
        if (tracing()) std::cout << "[damage] local hit observer enabled" << std::endl;
    } else if (!desired && installed) {
        runtime.register_function(kApplyDamage, original, original_name);
        installed = false;
        if (tracing()) std::cout << "[damage] local hit observer disabled" << std::endl;
    }
}
const DamageHistory &damage_history() {
    return history;
}
}
