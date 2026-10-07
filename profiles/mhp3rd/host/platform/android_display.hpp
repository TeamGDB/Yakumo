#pragma once

struct ANativeWindow;

namespace mhp3rd::android {

// A preference for this surface, not a system display setting. Android may
// choose a different rate for power, thermal or display-policy reasons.
// Returns false when unavailable (including Android 10) or rejected.
[[nodiscard]] bool request_display_frame_rate(ANativeWindow *window, float rate);

} // namespace mhp3rd::android
