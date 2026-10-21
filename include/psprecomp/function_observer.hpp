#pragma once

#include <cstdint>
#include <string>

namespace psprecomp {

class Runtime;
struct AllegrexContext;

// Told of every function registered with a Runtime, as it is registered:
// host code that layers over other host code at the same address learns here
// that something took the address over (a mod's hook under a host feature
// that replaces the game's function, for one). One observer at a time; it
// runs on the registering thread for each registration, so it must be cheap.
using FunctionObserver = void (*)(
    Runtime &runtime, std::uint32_t address, void (*function)(Runtime &, AllegrexContext &), const std::string &name);
void set_function_observer(FunctionObserver observer) noexcept;

} // namespace psprecomp
