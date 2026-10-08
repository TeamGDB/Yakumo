// Android's surface-rate ABI and graceful fallback, without a device or NDK.
#include "platform/android_display.hpp"

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
std::string scenario;
unsigned opens{}, lookups{}, requests{};
ANativeWindow *received_window{};
float received_rate{};

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

// Match the system loader's nonthrowing ABI; contract violations fail the test.
void require_loader(bool condition, const char *message) noexcept {
    if (condition) return;
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

std::int32_t set_rate(ANativeWindow *window, float rate, std::int8_t compatibility) {
    require(compatibility == 0, "games use DEFAULT frame-rate compatibility");
    ++requests;
    received_window = window;
    received_rate = rate;
    return scenario == "rejected" ? -22 : 0;
}
} // namespace

extern "C" void *yakumo_test_dlopen(const char *name, int flags) noexcept(noexcept(dlopen(nullptr, 0))) {
    require_loader(std::strcmp(name, "libandroid.so") == 0, "load the system Android library");
    require_loader(flags == (RTLD_NOW | RTLD_LOCAL), "keep library symbols local");
    ++opens;
    return scenario == "missing_library" ? nullptr : reinterpret_cast<void *>(1);
}

extern "C" void *yakumo_test_dlsym(void *library, const char *name) noexcept(noexcept(dlsym(nullptr, nullptr))) {
    require_loader(library == reinterpret_cast<void *>(1), "look up in the opened Android library");
    require_loader(std::strcmp(name, "ANativeWindow_setFrameRate") == 0, "resolve the API 30 symbol");
    ++lookups;
    return scenario == "missing_symbol" ? nullptr : reinterpret_cast<void *>(set_rate);
}

int main(int argc, char **argv) {
    try {
        require(argc == 2, "one isolated loader scenario is required");
        scenario = argv[1];
        unsetenv("MHP3RD_NO_DISPLAY_RATE_REQUEST");
        auto *first = reinterpret_cast<ANativeWindow *>(1);
        auto *second = reinterpret_cast<ANativeWindow *>(2);
        using mhp3rd::android::request_display_frame_rate;
        require(!request_display_frame_rate(nullptr, 120.0f), "missing surface is safe");
        for (float invalid :
            {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
            require(!request_display_frame_rate(first, invalid), "invalid target is rejected");
        require(opens == 0 && requests == 0, "invalid requests never load or call an Android API");
        if (scenario == "disabled") {
            setenv("MHP3RD_NO_DISPLAY_RATE_REQUEST", "1", 1);
            require(!request_display_frame_rate(first, 120.0f), "comparison switch restores the old behavior");
            require(opens == 0, "disabled request does not initialize the API");
            unsetenv("MHP3RD_NO_DISPLAY_RATE_REQUEST");
        }
        const bool available = scenario == "success" || scenario == "disabled";
        require(request_display_frame_rate(first, 120.0f) == available, "API failure is nonfatal");
        require(request_display_frame_rate(second, 90.0f) == available, "updated target applies to the new surface");
        require(opens == 1, "resolve the API once per process");
        require(lookups == (scenario == "missing_library" ? 0u : 1u), "missing Android library is safe");
        if (available || scenario == "rejected") {
            require(requests == 2 && received_window == second && received_rate == 90.0f,
                "pass the exact surface and current target through the NDK ABI");
        } else {
            require(requests == 0, "Android 10 without this symbol never calls it");
        }
        std::cout << "Android display-rate contracts passed: " << scenario << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
