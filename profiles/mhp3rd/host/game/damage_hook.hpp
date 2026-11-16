#pragma once
#include "psprecomp/function_registration.hpp"
namespace mhp3rd::game::combat {
class DamageHistory;
void observe_damage_registration(psprecomp::Runtime &, std::uint32_t, psprecomp::RegisteredFunction, std::string_view);
void damage_frame(psprecomp::Runtime &runtime);
[[nodiscard]] const DamageHistory &damage_history();
}
