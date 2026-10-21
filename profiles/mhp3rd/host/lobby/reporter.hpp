#pragma once

// The lobby reporter: sends the local hunter's data to a lobby dashboard when
// it enters a gathering hall (lobby/report.hpp has the wire format). It runs
// on a thread of its own and never blocks the game; the game thread only
// snapshots the data about once a second between frames.
//
// Off by default: with the lobby setting off or no server set, frame() does
// nothing at all.

#include <cstdint>
#include <string>

namespace psprecomp {
class Runtime;
}

namespace mhp3rd::lobby {

// Called between frames on the game thread, next to the other per-frame
// hooks. Starts the network thread the first time the lobby is enabled and
// queues reports as the hall or the hunter's data changes.
void frame(psprecomp::Runtime &runtime);

// What the menu shows.
struct Status {
    bool enabled{};
    bool connected{};
    std::string server;
    std::string last_error;
    std::uint64_t reports_sent{};
};
[[nodiscard]] Status status();

// Stops the network thread and waits for it. Called at exit; safe to call
// more than once.
void shutdown();

} // namespace mhp3rd::lobby
