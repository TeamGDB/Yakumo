#include "psprecomp/function_registration.hpp"
#include "psprecomp/runtime.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Entry {
    psprecomp::Runtime *runtime;
    std::uint32_t address;
    psprecomp::RegisteredFunction function;
    std::string name;
};
std::vector<Entry> seen;
unsigned nested{};
int failures{};
void check(bool yes, const char *message) {
    if (!yes) {
        ++failures;
        std::cerr << message << '\n';
    }
}
void function(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) {
    ctx.gpr[2] = 42u;
}
void observe(
    psprecomp::Runtime &runtime, std::uint32_t address, psprecomp::RegisteredFunction pointer, std::string_view name) {
    seen.push_back({&runtime, address, pointer, std::string(name)});
}
void other(psprecomp::Runtime &, std::uint32_t, psprecomp::RegisteredFunction, std::string_view) {
    ++nested;
}
void fail(psprecomp::Runtime &, std::uint32_t, psprecomp::RegisteredFunction, std::string_view) {
    throw std::runtime_error("fixture failure");
}
}
int main() {
    psprecomp::Runtime first(32u * 1024u * 1024u), second(32u * 1024u * 1024u);
    first.register_function(0x08800000u, &function, "before scope");
    check(seen.empty(), "registration without a scope is unchanged");
    {
        psprecomp::FunctionRegistrationScope scope(first, &observe);
        first.register_function(0x08800004u, &function, "original function name");
        second.register_function(0x08800008u, &function, "other runtime");
        check(seen.size() == 1 && seen[0].runtime == &first && seen[0].address == 0x08800004u &&
                seen[0].function == &function && seen[0].name == "original function name",
            "exact runtime, function and name observed");
        {
            psprecomp::FunctionRegistrationScope inner(first, &other);
            first.register_function(0x0880000Cu, &function, "nested");
        }
        first.register_function(0x08800010u, &function, "outer restored");
        check(nested == 1 && seen.size() == 2, "nested scopes restore the previous observer");
        try {
            psprecomp::FunctionRegistrationScope failing(first, &fail);
            first.register_function(0x08800014u, &function, "fails before registration");
        } catch (const std::runtime_error &) {
        }
        check(!first.has_function(0x08800014u), "observer exception leaves registration unmodified");
        first.register_function(0x08800018u, &function, "restored after exception");
        check(seen.size() == 3, "observer restored after exception");
        try {
            first.register_function(0x0880001Cu, nullptr, "invalid");
        } catch (const std::exception &) {
        }
        check(seen.size() == 3 && !first.has_function(0x0880001Cu), "invalid original never reaches observer");
        {
            psprecomp::FunctionRegistrationScope disabled(first, nullptr);
            first.register_function(0x08800020u, &function, "disabled");
        }
        check(seen.size() == 3, "explicit null observer suppresses only its own scope");
    }
    first.register_function(0x08800024u, &function, "after scope");
    check(seen.size() == 3, "observer is not retained after scope");
    psprecomp::AllegrexContext context;
    check(first.invoke_isolated_aot(0x08800004u, context) && context.gpr[2] == 42u,
        "observing does not replace the original executable function");
    return failures ? 1 : 0;
}
