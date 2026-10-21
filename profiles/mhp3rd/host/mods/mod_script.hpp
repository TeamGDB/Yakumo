#pragma once

#include "mods/mod_library.hpp"

#include <vector>

namespace psprecomp {
class Runtime;
}

// Script mods: Luau scripts that see the game through the same services as
// native mods (native_mods.hpp), exposed as the table `yakumo`. A script runs
// in a sandbox: no files, no os, globals read-only, a memory limit, and a time
// limit on every call so a script cannot stall a frame. A script that fails
// stops being called; the failure is on the Mods page and in the log.
namespace mhp3rd::mods::scripts {

// Runs each script's main chunk, in order. Once, after the native mods start.
void start(psprecomp::Runtime &runtime, const std::vector<Resolution::Source> &sources);

// How many scripts run now (started and not failed).
[[nodiscard]] std::size_t running();

// Whether the callback running now is out of time, as Luau's interrupt asks
// it: `gc_state` is -1 from the VM, which stops the script then, and 0 or
// more from the collector, which never does. The collector also runs while
// Yakumo sets a call up, outside any callback, when the last one's time is
// long gone; an error raised there would be unprotected.
[[nodiscard]] bool out_of_time(int gc_state);

// Forgets the scripts, so tests can start again.
void reset_for_tests();

} // namespace mhp3rd::mods::scripts
