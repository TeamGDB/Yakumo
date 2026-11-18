#include "audio/playback_watchdog.hpp"

#include <cstdlib>
#include <iostream>

namespace {
using mhp3rd::audio::PlaybackWatchdog;
using Millis = PlaybackWatchdog::Millis;

int failures{};
void expect(bool ok, const char *what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

constexpr PlaybackWatchdog::State kPlaying{true, false, true};
constexpr PlaybackWatchdog::State kBackground{true, false, false};
constexpr PlaybackWatchdog::State kPaused{true, true, true};
constexpr PlaybackWatchdog::State kNoDevice{false, false, true};

// A device that keeps asking for frames is never reopened.
void playing_device_left_alone() {
    PlaybackWatchdog watchdog;
    for (Millis now = 1000; now < 60'000; now += 16)
        expect(!watchdog.due(now, now - 10, kPlaying), "a device asking for frames is not reopened");
    expect(watchdog.reopens() == 0u, "no reopen while playing");
}

// The regression: the app returns from the background, the system refuses to
// restart the stolen stream and the device never asks for frames again. Before
// this, the game stayed silent until it was restarted.
void stolen_stream_after_background_is_reopened() {
    PlaybackWatchdog watchdog;
    Millis now = 1000;
    for (; now < 5000; now += 16) expect(!watchdog.due(now, now - 5, kPlaying), "playing before the background");
    const Millis last_feed = now - 5;
    // Thirty seconds in another app: the device is silent on purpose.
    for (; now < 35'000; now += 100)
        expect(!watchdog.due(now, last_feed, kBackground), "a device in the background is not reopened");
    // Back in the foreground; the old request is from before the background,
    // so the device gets the full stall time to start again.
    const Millis back = now;
    expect(!watchdog.due(back, last_feed, kPlaying), "returning to the foreground is not itself a stall");
    expect(!watchdog.due(back + PlaybackWatchdog::kStallMillis - 1, last_feed, kPlaying),
        "the device gets the stall time to restart after the background");
    expect(watchdog.due(back + PlaybackWatchdog::kStallMillis, last_feed, kPlaying),
        "a device still silent after the stall time is reopened");
    watchdog.reopened(back + PlaybackWatchdog::kStallMillis);
    expect(watchdog.reopens() == 1u, "the reopen is counted");
    // The new stream plays.
    const Millis reopened = back + PlaybackWatchdog::kStallMillis;
    for (now = reopened + 10; now < reopened + 10'000; now += 16)
        expect(!watchdog.due(now, now - 3, kPlaying), "the reopened device that plays is left alone");
    expect(watchdog.reopens() == 1u, "one reopen fixes it");
}

// A device that returns within the stall time, as it normally does, is fine.
void normal_restart_is_not_a_stall() {
    PlaybackWatchdog watchdog;
    expect(!watchdog.due(1000, 990, kPlaying), "playing");
    expect(!watchdog.due(2000, 990, kBackground), "background");
    expect(!watchdog.due(9000, 990, kPlaying), "foreground again");
    // The device restarts 150 ms later, as AAudio did on the test phone.
    for (Millis now = 9150; now < 20'000; now += 16)
        expect(!watchdog.due(now, now - 2, kPlaying), "a device restarted in time is not reopened");
    expect(watchdog.reopens() == 0u, "no reopen for a normal restart");
}

// The paused menu stops the device on purpose, however long it stays open.
void paused_device_left_alone() {
    PlaybackWatchdog watchdog;
    expect(!watchdog.due(1000, 995, kPlaying), "playing before the menu");
    for (Millis now = 1000; now < 120'000; now += 16)
        expect(!watchdog.due(now, 995, kPaused), "a device paused by the menu is not reopened");
    // Closing the menu gives the device the stall time again.
    expect(!watchdog.due(120'000, 995, kPlaying), "closing the menu is not a stall");
    expect(!watchdog.due(120'000 + PlaybackWatchdog::kStallMillis - 1, 995, kPlaying),
        "the device gets the stall time after the menu");
    expect(watchdog.due(120'000 + PlaybackWatchdog::kStallMillis, 995, kPlaying),
        "a device still silent after the menu is reopened");
}

// A device that stays silent after reopening is tried again, less and less
// often, and never in a tight loop.
void silent_device_backs_off() {
    PlaybackWatchdog watchdog;
    const Millis last_feed = 1000;
    expect(!watchdog.due(1000, last_feed, kPlaying), "start watching");
    Millis now = 1000 + PlaybackWatchdog::kStallMillis;
    Millis expected_delay = PlaybackWatchdog::kFirstRetryMillis;
    for (int attempt = 0; attempt < 10; ++attempt) {
        expect(watchdog.due(now, last_feed, kPlaying), "a silent device is reopened");
        watchdog.reopened(now);
        expect(!watchdog.due(now + expected_delay - 1, last_feed, kPlaying), "no reopen before the retry delay");
        now += expected_delay;
        expected_delay = expected_delay * 2 > PlaybackWatchdog::kMaxRetryMillis ? PlaybackWatchdog::kMaxRetryMillis
                                                                                : expected_delay * 2;
    }
    expect(expected_delay == PlaybackWatchdog::kMaxRetryMillis, "the retry delay is capped");
    expect(watchdog.reopens() == 10u, "every retry is counted");
    // Once it plays, the next failure starts again from the shortest delay.
    expect(!watchdog.due(now, now - 1, kPlaying), "the device plays again");
    const Millis stall = now + PlaybackWatchdog::kStallMillis;
    expect(watchdog.due(stall, now - 1, kPlaying), "a later stall is reopened");
    watchdog.reopened(stall);
    expect(watchdog.due(stall + PlaybackWatchdog::kFirstRetryMillis, now - 1, kPlaying),
        "after playing again the retry delay starts over");
}

// Opening failed (the stream was taken during start-up): open again, with the
// same back-off, but only while the game is in the foreground.
void missing_device_opened_again() {
    PlaybackWatchdog watchdog;
    expect(!watchdog.due(1000, 0, PlaybackWatchdog::State{false, false, false}), "no attempt while in the background");
    expect(watchdog.due(2000, 0, kNoDevice), "a device that could not be opened is tried again");
    watchdog.reopened(2000);
    expect(!watchdog.due(2000 + PlaybackWatchdog::kFirstRetryMillis - 1, 0, kNoDevice), "not again at once");
    expect(watchdog.due(2000 + PlaybackWatchdog::kFirstRetryMillis, 0, kNoDevice), "again after the delay");
    expect(!watchdog.due(9000, 0, PlaybackWatchdog::State{false, true, true}), "no attempt while paused");
}

} // namespace

int main() {
    playing_device_left_alone();
    stolen_stream_after_background_is_reopened();
    normal_restart_is_not_a_stall();
    paused_device_left_alone();
    silent_device_backs_off();
    missing_device_opened_again();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "playback watchdog tests passed\n";
    return EXIT_SUCCESS;
}
