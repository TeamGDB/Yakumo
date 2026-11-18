#include "audio/playback_watchdog.hpp"

#include <algorithm>

namespace mhp3rd::audio {

bool PlaybackWatchdog::due(Millis now, Millis last_feed, const State &state) {
    // Paused or in the background, the device is meant to be silent.
    if (state.paused || !state.focused) {
        watching_ = false;
        return false;
    }
    // The device was idle until now on purpose: its last request is old news.
    if (!watching_) {
        watching_ = true;
        grace_start_ = now;
    }
    if (state.device_open) {
        const Millis since = now - std::max(last_feed, grace_start_);
        if (since < kStallMillis) {
            // Playing: the next failure starts over with the shortest wait.
            retry_delay_ = kFirstRetryMillis;
            next_attempt_ = 0;
            return false;
        }
    }
    // Stopped, or no stream at all after a failed open.
    return now >= next_attempt_;
}

void PlaybackWatchdog::reopened(Millis now) {
    ++reopens_;
    // A new stream needs a moment before its first request.
    grace_start_ = now;
    next_attempt_ = now + retry_delay_;
    retry_delay_ = std::min(retry_delay_ * 2, kMaxRetryMillis);
}

} // namespace mhp3rd::audio
