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
// Asks for a driver package .zip (as Winlator, Skyline and the Adreno Tools
// driver repositories distribute them: meta.json and a main .so, often with
// further .so's it depends on) and extracts every .so in it into the app's
// private storage, where adrenotools can load them from (a content:// zip is
// not a path it can open). The main driver is the only .so found, or, among
// several, the one whose name holds "vulkan"; meta.json's "libraryName" is
// not read (ponytail: parse it instead, if a package with several unrelated
// .so's and none named "vulkan" ever needs it). Nothing when the player
// cancels.
[[nodiscard]] std::optional<PickedDriver> pick_custom_gpu_driver();

// Removes a driver pick_custom_gpu_driver() installed, so "System default"
// starts clean the next time one is picked.
void clear_custom_gpu_driver();

// `library`'s driver package's meta.json "name", for the Video tab's row
// (Winlator and Skyline show this instead of the bare file name); `library`
// itself when there is no meta.json or it cannot be read, empty when
// `library` is.
[[nodiscard]] std::string driver_display_name(const std::string &library);

// Opens `library` (as pick_custom_gpu_driver named it) through adrenotools:
// a dlopen-like handle for libvulkan.so whose driver is redirected to it, to
// pass to volkInitializeCustom() (through its vkGetInstanceProcAddr). Null
// and an error on any failure (the file is gone, the device refuses the
// hook), for the caller to fall back to the phone's own driver. The caller
// owns the handle (dlclose when done).
[[nodiscard]] void *open_custom_gpu_driver(const std::string &library, std::string &error);

} // namespace mhp3rd::android
