# volk

`volk.h` and `volk.c` are unmodified copies of [volk](https://github.com/zeux/volk) at commit `7f46f79751d7e3b3a6df20e38d3e3986585bcdf4`, under the MIT License (see `LICENSE.md`).

A Vulkan meta-loader: it declares `vk*` as function pointers of the same names instead of symbols resolved at link time, so the Android app can back them with a dlopen-like handle other than the system `libvulkan.so` (see `host/platform/android_gpu_driver.hpp`) without rewriting its Vulkan call sites. Desktop and other platforms keep linking `libvulkan` directly, as before; volk is compiled in for the Android app only (`profiles/mhp3rd/CMakeLists.txt`, `MHP3RD_ANDROID_APP`).
