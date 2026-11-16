#pragma once
#include <cstdint>
#include <string_view>

namespace psprecomp {
class Runtime;
struct AllegrexContext;
using RegisteredFunction = void (*)(Runtime &, AllegrexContext &);
using FunctionRegistrationObserver = void (*)(Runtime &, std::uint32_t, RegisteredFunction, std::string_view);

// Observe registration while a profile installs a dynamic code module. The
// observer only borrows the function/name; it must not modify registration.
// Scopes nest and restore their predecessor on exception. Like registration
// itself, scopes are confined to the thread that owns the Runtime.
class FunctionRegistrationScope {
public:
    FunctionRegistrationScope(Runtime &runtime, FunctionRegistrationObserver observer);
    ~FunctionRegistrationScope();
    FunctionRegistrationScope(const FunctionRegistrationScope &) = delete;
    FunctionRegistrationScope &operator=(const FunctionRegistrationScope &) = delete;

private:
    Runtime *previous_runtime_;
    FunctionRegistrationObserver previous_observer_;
};
} // namespace psprecomp
