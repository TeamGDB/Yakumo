#pragma once

#include "debug/quest_start.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace mhp3rd::debug::encounter {

// A deliberately narrow NPJB-40001 preset: the original quest creates its
// actors/resources. The tools select its initial area and relocate the hunter
// only after the real Barioth has landed. No actor ID, HP or AI is changed.
inline constexpr std::uint32_t kInitialAreaChoice = 0x088BDEECu;
inline constexpr std::uint32_t kInitialAreaReturn = 0x088C1EDCu;
inline constexpr std::uint16_t kQuest = 507u;

enum class Phase { Idle, Village, Departing, Loading, Camera, Ready, Finished, Failed };
enum class Action { None, Focus };

struct View {
    std::string reason;
    std::uint32_t actor{};
    std::uint32_t model{};
    std::uint32_t population{};
    std::uint32_t ticks{};
    std::array<float, 3> target{};
    std::array<float, 3> hunter{};
    bool in_view{};
    [[nodiscard]] bool valid() const { return reason.empty(); }
};

[[nodiscard]] bool supported(const game::Ram &ram);
[[nodiscard]] View inspect(const game::Ram &ram);

class Controller {
public:
    // All calls run on the guest thread. begin/tick are reached through the
    // existing frame-boundary queue. initial_choice completes the game's real
    // initial-area call; its only write precedes map/actor initialization.
    std::string begin(const game::Ram &ram, const quests::Quest &quest, std::uint64_t now_ms);
    Action tick(game::Ram &ram, std::uint64_t now_ms, bool writes_allowed);
    bool initial_choice(game::Ram &ram, std::uint32_t return_pc, bool resumed, bool writes_allowed);
    void cancel();
    [[nodiscard]] Phase phase() const { return phase_; }
    [[nodiscard]] std::string status() const;

private:
    void fail(std::string reason);
    Phase phase_{Phase::Idle};
    quests::Quest quest_;
    View view_;
    std::string reason_;
    std::uint64_t deadline_{};
    std::uint64_t stable_since_{};
    std::uint32_t first_ticks_{};
    std::uint32_t counted_ticks_{}; // quest clock of the last counted stable frame
    unsigned stable_frames_{};
    float previous_height_{};
};

} // namespace mhp3rd::debug::encounter
