#pragma once

// A custom Vulkan ICD the player can pick instead of the phone's own vendor
// driver (a Turnip/Mesa build for their Adreno GPU), as Winlator, Skyline and
// yuzu-Android already let players do: adrenotools (github.com/bylaws/
// libadrenotools) opens a dlopen-like handle for libvulkan.so with the
// driver it loads internally redirected to a chosen file. Only in the
// Android app (MHP3RD_ANDROID_APP); gpu/vulkan_renderer.cpp drives this at
// start-up, and host/ui/menu.cpp's Video tab lets the player pick and clear
// the driver.

#include <filesystem>
#include <optional>
#include <string>

namespace mhp3rd::android {

struct PickedDriver {
    std::string library;  // the main driver's file name (settings.custom_gpu_driver); empty on error
    std::string error;    // empty: picking and installing it worked
};
// Asks for a folder holding an extracted Adreno driver package (see
// libadrenotools' tools/ADPKG.md: a main .so and, often, further .so's it
// depends on) and copies every .so in it into the app's private storage,
// where adrenotools can load them from (a content:// folder is not a path it
// can open). The main driver is the only .so found, or, among several, the
// one whose name holds "vulkan"; meta.json's "libraryName" is not read
// (ponytail: parse it instead, if a package with several unrelated .so's and
// none named "vulkan" ever needs it). Nothing when the player cancels.
[[nodiscard]] std::optional<PickedDriver> pick_custom_gpu_driver();

// Removes a driver pick_custom_gpu_driver() installed, so "System default"
// starts clean the next time one is picked.
void clear_custom_gpu_driver();

// Opens `library` (as pick_custom_gpu_driver named it) through adrenotools:
// a dlopen-like handle for libvulkan.so whose driver is redirected to it, to
// pass to volkInitializeCustom() (through its vkGetInstanceProcAddr). Null
// and an error on any failure (the file is gone, the device refuses the
// hook), for the caller to fall back to the phone's own driver. The caller
// owns the handle (dlclose when done).
[[nodiscard]] void *open_custom_gpu_driver(const std::string &library, std::string &error);

} // namespace mhp3rd::android
