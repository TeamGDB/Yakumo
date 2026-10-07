#include "android_display.hpp"

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace mhp3rd::android {
namespace {

// ANativeWindow_setFrameRate is available from API 30. Resolve it at run
// time so the same APK still loads on Android 10 (API 29). Signature and
// compatibility value follow the Android NDK native_window.h contract.
using SetRate = std::int32_t (*)(ANativeWindow *, float, std::int8_t);

SetRate load_set_rate() {
    void *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (library == nullptr) return nullptr;
    // Keep the library loaded while its function pointer is in use.
    return reinterpret_cast<SetRate>(dlsym(library, "ANativeWindow_setFrameRate"));
}

} // namespace

bool request_display_frame_rate(ANativeWindow *window, float rate) {
    if (window == nullptr || !std::isfinite(rate) || rate <= 0.0f) return false;
    if (std::getenv("MHP3RD_NO_DISPLAY_RATE_REQUEST") != nullptr) return false;
    static const SetRate set_rate = load_set_rate();
    // DEFAULT compatibility is for games, unlike FIXED_SOURCE for video.
    return set_rate != nullptr && set_rate(window, rate, 0) == 0;
}

} // namespace mhp3rd::android
