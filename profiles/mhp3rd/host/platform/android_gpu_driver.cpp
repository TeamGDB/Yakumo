#include "platform/android_gpu_driver.hpp"

#include "app_paths.hpp"
#include "platform/android_jni.hpp"

#include <adrenotools/driver.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <fstream>
#include <vector>

namespace mhp3rd::android {
namespace {

namespace fs = std::filesystem;

bool copy_document_to_file(const std::string &uri, const fs::path &target) {
    const int fd = open_document(uri, "r");
    if (fd < 0) return false;
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    std::array<char, 1 << 16> buffer{};
    bool ok = static_cast<bool>(out);
    while (ok) {
        const ssize_t got = ::read(fd, buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            ok = got == 0;
            break;
        }
        out.write(buffer.data(), got);
        ok = static_cast<bool>(out);
    }
    ::close(fd);
    return ok;
}

// Where installed driver files live: the app's private storage, which
// adrenotools insists on (not removable storage, which any app could tamper
// with; see adrenotools_open_libvulkan's customDriverDir).
fs::path driver_directory() {
    const char *storage = SDL_GetAndroidInternalStoragePath();
    return storage == nullptr ? fs::path{} : fs::path(storage) / "gpu_driver";
}

} // namespace

std::optional<PickedDriver> pick_custom_gpu_driver() {
    const std::optional<std::string> tree = pick_folder();
    if (!tree) return std::nullopt;
    PickedDriver picked;
    const fs::path directory = driver_directory();
    if (directory.empty()) {
        picked.error = "no private storage to copy the driver into";
        return picked;
    }
    const auto entries = list_folder(tree_root(*tree));
    if (!entries) {
        picked.error = "Android would not let Yakumo read that folder.";
        return picked;
    }
    std::error_code ec;
    fs::remove_all(directory, ec);
    fs::create_directories(directory, ec);
    std::vector<std::string> libraries;  // every .so copied, to pick the main one among them
    for (const Entry &entry : *entries) {
        if (entry.directory || !entry.name.ends_with(".so")) continue;
        if (copy_document_to_file(entry.uri, directory / entry.name)) libraries.push_back(entry.name);
    }
    if (libraries.empty()) {
        picked.error = "no .so file in the chosen folder";
        fs::remove_all(directory, ec);
        return picked;
    }
    // The main driver: the only one, or, among several, the one naming
    // "vulkan" (libadrenotools' tools/ADPKG.md calls it meta.json's
    // "libraryName", which is not read here).
    picked.library = libraries.front();
    if (libraries.size() > 1u)
        for (const std::string &name : libraries)
            if (name.find("vulkan") != std::string::npos) {
                picked.library = name;
                break;
            }
    return picked;
}

void clear_custom_gpu_driver() {
    std::error_code ec;
    fs::remove_all(driver_directory(), ec);
}

void *open_custom_gpu_driver(const std::string &library, std::string &error) {
    const fs::path directory = driver_directory();
    std::error_code ec;
    if (directory.empty() || !fs::exists(directory / library, ec)) {
        error = "the installed driver is missing; pick it again";
        return nullptr;
    }
    // adrenotools' hookLibDir: the app's nativeLibraryDir, where build_apk.sh
    // packs its hook libraries (libmain_hook.so, libhook_impl.so) beside
    // libmain.so itself.
    const fs::path hooks = executable_directory();
    const std::string driver_dir = directory.string() + "/";
    void *handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, /*tmpLibDir=*/nullptr,
                                              hooks.c_str(), driver_dir.c_str(), library.c_str(),
                                              /*fileRedirectDir=*/nullptr, /*userMappingHandle=*/nullptr);
    if (handle == nullptr)
        error = "adrenotools could not load the driver (an old Android version, or a device it does not support)";
    return handle;
}

} // namespace mhp3rd::android
