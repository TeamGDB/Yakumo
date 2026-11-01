#pragma once
#include <cstdint>
namespace mhp3rd::game { class Ram; }
namespace mhp3rd::game::control_context {
enum class Scene : std::uint8_t { Unknown, Hub, Quest };
struct Context {
    Scene scene{Scene::Unknown};
    bool interface_open{true};
    bool operator==(const Context &) const = default;
};
// Read-only snapshot at the original game flip. Unknown/loading/title states
// use interface controls; no input, camera or HUD visibility is inferred.
[[nodiscard]] Context read(const Ram &ram);
bool refresh(const Ram &ram);
[[nodiscard]] Context current();
} // namespace mhp3rd::game::control_context
