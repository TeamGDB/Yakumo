#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace mhp3rd::settings {
enum class FrameRate;
}

// When frame interpolation presents, what each present shows, and how many
// presents a second the machine can afford. Independent of the renderer and
// of the clock, so it can be checked on made-up timings.
//
// The game makes a frame every two vblanks of its 59.94 Hz display, 33.4 ms
// of emulated time. Each frame belongs to a moment of real time: the moment
// the kernel's hold to real time maps the flip's emulated time to. That
// moment is steady even when the game's code takes longer on some frames than
// on others, which the real time of the flip itself is not.
//
// Presents follow a grid of evenly spaced moments, `presents per frame` of
// them per game frame, aligned with the frames' moments: at 60 every frame's
// moment and halfway between; at 45 every other frame's moment and a third of
// the way into the ones between. Each grid moment is shown by a present made
// a fixed delay later, blending the two frames around the moment by where it
// falls between them. The delay is as short as it can be: a frame reaches the
// renderer when the game's code for it has run, some milliseconds after its
// moment, and the first grid moment past a frame's moment needs that frame.
// So the delay is the frame time less the shortest step from a frame's moment
// to the next grid moment, plus the time the game's code takes: the ninth
// longest of the last 60 frames, and a millisecond. A frame later than that,
// a stall or a load, holds the newest picture for a present or two rather
// than keep every present late for two seconds. At 60 a frame is on screen as
// it is 16.7 ms plus that time after its moment; without interpolation it is
// shown when its code has run.
namespace mhp3rd::gpu::pacing {

// Two vblanks of the PSP's display, in microseconds (kernel kVBlankPeriodUs).
inline constexpr std::int64_t kGameFrameUs = 2 * 16'683;

// The rates the Frame rate setting offers, slowest first; 30 is the game's
// own, without interpolation. A display rate outside these joins them.
inline constexpr double kRates[] = {30.0, 45.0, 60.0, 90.0, 120.0};

// The player's target before Vsync or the governor caps it. Display mode
// follows the known refresh rate, or 60 when the display cannot report one.
[[nodiscard]] double requested_rate(settings::FrameRate rate, double display_hz) noexcept;

// Presents per game frame at `rate` presents a second: 45 is 1.5, 90 is 3.
[[nodiscard]] double presents_per_frame(double rate) noexcept;
// Of those, the ones that fall on a frame's own moment and show it as it
// is, per game frame: 1 at 60, 90 and 120, 0.5 at 45, 0.2 at 144.
[[nodiscard]] double plain_presents_per_frame(double rate) noexcept;

class PresentClock {
public:
    // Presents per game frame; 1 or less stops the clock.
    void set_presents_per_frame(double presents) noexcept;
    [[nodiscard]] double presents_per_frame() const noexcept { return per_frame_; }
    // A new game frame, for the moment `time_us` of real time, flipped at
    // `now_us`. True when the frames started over: a gap of a load, or a
    // moment out of order.
    bool flip(std::int64_t time_us, std::int64_t now_us) noexcept;
    // Forgets the frames and the grid, as after a pause.
    void reset() noexcept;

    // The time of the next present, once there is a frame to show.
    [[nodiscard]] std::optional<std::int64_t> next_due() const noexcept;
    struct Present {
        std::int64_t time_us{};  // the grid moment it was due at
        float t{};               // 0 shows the older frame, 1 the newer one
        std::uint32_t skipped{}; // grid moments passed over since the last present
    };
    // The latest present due at `now_us`, if one is; it counts as done.
    // Earlier ones not presented in time are skipped.
    [[nodiscard]] std::optional<Present> take(std::int64_t now_us) noexcept;
    // Where a present at grid moment `time_us` falls between the two frames.
    [[nodiscard]] float blend_at(std::int64_t time_us) const noexcept;

    [[nodiscard]] std::int64_t frame_us() const noexcept { return frame_us_; }
    // From a frame's moment to the present that shows it as it is.
    [[nodiscard]] std::int64_t delay_us() const noexcept;
    // The time the game's code takes after a frame's moment, as the delay
    // allows for it.
    [[nodiscard]] std::int64_t work_us() const noexcept { return work_us_; }

    static constexpr std::int64_t kWorkMarginUs = 1000;
    static constexpr std::size_t kWorkWindowFrames = 60;
    static constexpr std::size_t kWorkRank = 8; // how many of the window may be later

private:
    // The grid moment `index`, and when it is presented.
    [[nodiscard]] std::int64_t moment(std::int64_t index) const noexcept;
    [[nodiscard]] std::int64_t due(std::int64_t index) const noexcept { return moment(index) + delay_us(); }

    double per_frame_{1.0};
    // The shortest step from a frame's moment to the next grid moment.
    std::int64_t first_step_us_{kGameFrameUs};
    std::int64_t work_us_{kWorkMarginUs};
    std::vector<std::int64_t> work_window_; // the last frames' code time, oldest first
    std::size_t work_cursor_{};
    std::int64_t frame_us_{kGameFrameUs};
    bool has_newer_{};
    bool has_older_{};
    std::int64_t newer_us_{};
    std::int64_t older_us_{};
    bool anchored_{};
    std::int64_t anchor_us_{};
    std::int64_t next_index_{};
};

// What one second of play measured, for the governor.
struct Second {
    double speed{1.0}; // emulated time per real time
    double idle_ms{};  // per game frame: the kernel waited with nothing to do
    double blend_ms{}; // CPU time of one blended present (replay, submit, present)
    double plain_ms{}; // CPU time of one present of a frame as it is
    // Of those, the presents' own work: without their waits for the GPU or
    // the display, which grow at a slower rate when the GPU lowers its clock.
    // Negative: not measured, the times above are used.
    double blend_work_ms{-1.0};
    double plain_work_ms{-1.0};
    double interpolation_ms{}; // per game frame: CPU time of all presents between flips
    std::uint32_t presents{};  // presents between flips this second
    std::uint32_t skipped{};   // grid moments passed over this second
    std::uint32_t blocked{};   // presents dropped: the display had no image free
};

// Picks the rate from the setting down to 30, so that interpolation never
// slows the game: when the game falls behind real time, or has no spare time
// left, and presents take at least half of the time missing and more than
// the time the kernel spent waiting, it steps down at once to a rate the
// measured costs say fits; when presents keep coming late, or the display has
// no image free for them, in most of the last few seconds, it steps down one
// rate. When the game has had spare time for a while, it
// goes up to the fastest rate the costs say fits. A rate that had to be left
// is not tried again for a while.
//
// Late presents count only while the game runs at its own speed, and not in
// the first seconds after the rate changed or the frames started over: a load
// catching up, the first presents of a new rate and a title scene's long
// stretches of the game's code are not what the rate costs in play. A burst
// of GPU work (a monster's breath close to the camera) makes a second or
// three late; a display slower than the rate makes every one. A rate left
// once because its presents came late is tried again after a load or a
// pause; one left again, or left because the display had no image free,
// waits out its block. Whether a faster rate fits is judged by what the
// presents cost the game, waits included, except once after such a load:
// then by their own work, since their waits grow when a slower rate lets
// the GPU lower its clock, and a GPU that cannot take the rate fails that
// one try and is blocked again.
class RateGovernor {
public:
    // The rate the setting asks for; the governor starts there.
    void set_requested(double rate);
    // The game's frames started over: a load, a pause or the menu. Not a
    // change of the rate, nor the governor's own step down to 30.
    void restart();
    // Off, the rate stays the one asked for whatever the game's speed
    // (Video > Lower the frame rate when the game falls behind); on again,
    // it starts from there.
    void set_automatic(bool automatic);
    [[nodiscard]] bool automatic() const noexcept { return automatic_; }
    [[nodiscard]] double rate() const noexcept;
    [[nodiscard]] double requested() const noexcept { return requested_; }
    // Once a second while the game runs; true when the rate changed, with the
    // reason in reason().
    bool update(const Second &second);
    [[nodiscard]] const char *reason() const noexcept { return reason_; }
    // CPU time a game frame spends presenting at `rate`, from `second`'s
    // costs; `work`: the presents' own work, without their waits.
    [[nodiscard]] static double cost_ms(double rate, const Second &second, bool work = false) noexcept;

    // Tuning, public for the tests.
    static constexpr double kSlowSpeed = 0.97;     // behind real time
    static constexpr double kFarBehindSpeed = 0.9; // behind enough to drop at once to the rate that fits
    static constexpr double kSteadySpeed = 0.99;   // keeping up
    static constexpr double kFastSpeed = 1.03;     // ahead: a load catching up
    static constexpr double kMinCostMs = 0.5;      // presents cost something worth saving
    static constexpr double kMinIdleMs = 1.0;      // spare time a frame needs to keep its moments
    static constexpr double kMarginMs = 3.0;       // spare time kept at a faster rate
    static constexpr int kSecondsBeforeUp = 3;
    static constexpr int kSecondsAfterDown = 10;
    static constexpr int kSettleSeconds = 2; // after a change or a restart, late presents do not count
    static constexpr int kBlockSeconds = 30;
    static constexpr int kMaxBlockSeconds = 300;
    static constexpr double kMaxSkippedShare = 0.1;
    static constexpr int kLateWindow = 5;  // the seconds late presents are counted over
    static constexpr int kLateSeconds = 4; // late ones among them that step down

private:
    // `late`: left because presents came late, which a restart may forgive.
    void step_to(std::size_t index, const char *why, bool late = false);

    double requested_{30.0};
    bool automatic_{true};
    std::vector<double> ladder_{30.0};
    std::size_t index_{};
    int slow_seconds_{};
    std::uint32_t late_seconds_{}; // the last kLateWindow seconds, newest in bit 0: presents came late
    bool trial_{};                 // a restart lifted a block: the next climb weighs the presents' work
    int spare_seconds_{};
    int wait_up_{};
    int settle_{};
    std::vector<int> blocked_;      // seconds before each rate may be tried again
    std::vector<int> block_length_; // how long the next block of each rate lasts
    std::vector<bool> late_block_;  // each rate's block came from late presents
    std::vector<int> late_steps_;   // how often each rate was left for late presents
    const char *reason_{""};
};

} // namespace mhp3rd::gpu::pacing
