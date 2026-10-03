#include "platform/android_gpu_driver.hpp"

#include "app_paths.hpp"
#include "platform/android_jni.hpp"

#include <adrenotools/driver.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <dlfcn.h>

#include <fstream>
#include <iterator>
#include <vector>

namespace mhp3rd::android {
namespace {

namespace fs = std::filesystem;

// Where installed driver files live: the app's private storage, which
// adrenotools insists on (not removable storage, which any app could tamper
// with; see adrenotools_open_libvulkan's customDriverDir).
fs::path driver_directory() {
    const char *storage = SDL_GetAndroidInternalStoragePath();
    return storage == nullptr ? fs::path{} : fs::path(storage) / "gpu_driver";
}

} // namespace

std::optional<PickedDriver> pick_custom_gpu_driver() {
    const std::optional<std::string> document = pick_document();
    if (!document) return std::nullopt;
    PickedDriver picked;
    const fs::path directory = driver_directory();
    if (directory.empty()) {
        picked.error = "no private storage to install the driver into";
        return picked;
    }
    std::error_code ec;
    fs::remove_all(directory, ec);
    fs::create_directories(directory, ec);
    const std::optional<std::vector<std::string>> libraries = install_gpu_driver_zip(*document, directory.string());
    if (!libraries) {
        picked.error = "Android would not let Yakumo read that file.";
        return picked;
    }
    if (libraries->empty()) {
        picked.error = "not a driver package: no .so file in that .zip";
        fs::remove_all(directory, ec);
        return picked;
    }
    // The main driver: the only one, or, among several, the one naming
    // "vulkan" (libadrenotools' tools/ADPKG.md calls it meta.json's
    // "libraryName", which is not read here).
    picked.library = libraries->front();
    if (libraries->size() > 1u)
        for (const std::string &name : *libraries)
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

std::string driver_display_name(const std::string &library) {
    if (library.empty()) return {};
    std::ifstream in(driver_directory() / "meta.json", std::ios::binary);
    if (!in) return library;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // A hand-rolled read of one string field, not a JSON parser: meta.json is
    // a flat object (schemaVersion, name, description, ..., libraryName) with
    // no nesting, and "name" is the only field this needs.
    std::size_t at = text.find("\"name\"");
    at = at == std::string::npos ? at : text.find(':', at + 6u);
    at = at == std::string::npos ? at : text.find('"', at);
    if (at == std::string::npos) return library;
    std::string name;
    for (++at; at < text.size() && text[at] != '"'; ++at) {
        if (text[at] == '\\' && at + 1u < text.size()) ++at; // skip the escape, keep the escaped character
        name += text[at];
    }
    return name.empty() ? library : name;
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
