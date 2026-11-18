#pragma once

#include <cstdint>

namespace mhp3rd::audio {

// Decides when the playback device has stopped asking for audio although it
// should be playing, so the sink can close and reopen it.
//
// Android can take a low-latency (MMAP) AAudio stream away from an app while
// the app is in the background, for example when another app plays a system
// sound. SDL pauses the stream when the app leaves the screen and restarts it
// when the app returns; a stolen stream refuses the restart
// (AAUDIO_ERROR_DISCONNECTED), SDL only logs that, and its device callback is
// never called again, so the game stays silent until it is restarted. Opening
// the device fails the same way when the stream is taken during start-up.
//
// The device asks for frames every few milliseconds while it plays, so a gap
// of half a second while the sink should be playing means it has stopped. The
// clock is the caller's, in milliseconds, which keeps this testable without a
// device or real time.
class PlaybackWatchdog {
public:
    using Millis = std::int64_t;

    // How long the device may go without asking for frames before it counts
    // as stopped.
    static constexpr Millis kStallMillis = 500;
    // The wait after a reopen before the next one, doubling while the device
    // stays silent, up to the maximum.
    static constexpr Millis kFirstRetryMillis = 1000;
    static constexpr Millis kMaxRetryMillis = 30'000;

    struct State {
        bool device_open{}; // a playback stream is open
        bool paused{};      // the sink paused the device on purpose (the paused menu)
        bool focused{};     // the window is in the foreground
    };

    // True when the device should be reopened now. `last_feed` is when the
    // device last asked for frames.
    [[nodiscard]] bool due(Millis now, Millis last_feed, const State &state);
    // Records a reopen at `now`, whether or not a stream could be opened: a
    // device that stays silent, or cannot be opened, is tried again later.
    void reopened(Millis now);

    // The number of reopens so far, for the log and tests.
    [[nodiscard]] std::uint32_t reopens() const { return reopens_; }

private:
    bool watching_{};      // the sink should be playing, as of the last call
    Millis grace_start_{}; // when it began to, or was last reopened
    Millis next_attempt_{};
    Millis retry_delay_{kFirstRetryMillis};
    std::uint32_t reopens_{};
};

} // namespace mhp3rd::audio
