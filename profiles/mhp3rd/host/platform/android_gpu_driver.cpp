#include "platform/android_gpu_driver.hpp"

#include "app_paths.hpp"
#include "platform/android_jni.hpp"
#include "platform/driver_meta.hpp"

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

// The text of `directory`'s meta.json; empty when there is none.
std::string read_meta_json(const fs::path &directory) {
    std::ifstream in(directory / "meta.json", std::ios::binary);
    return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string{};
}

// The Video tab asks for the driver's name on every frame it draws; the file
// is read once per driver, until a pick or a clear changes what is installed.
struct DisplayNameCache {
    std::string library;
    std::string name;
    bool valid{};
};
DisplayNameCache &display_name_cache() {
    static DisplayNameCache cache;
    return cache;
}

// A pick is extracted here first and only moved over driver_directory() once
// it is known to be a driver package, so a bad pick leaves the working driver
// (and the setting that names it) as they were.
fs::path staging_directory() {
    fs::path directory = driver_directory();
    return directory.empty() ? directory : directory.parent_path() / "gpu_driver.new";
}

// Present from the moment a custom driver is first used until the renderer is
// up with it: finding it at the next start means that attempt failed or the
// driver crashed the app, and the player never reached the Video tab to undo it.
fs::path trial_marker() {
    return driver_directory() / ".trial";
}

} // namespace

std::optional<PickedDriver> pick_custom_gpu_driver() {
    const std::optional<std::string> document = pick_document();
    if (!document) return std::nullopt;
    PickedDriver picked;
    const fs::path directory = driver_directory();
    const fs::path staging = staging_directory();
    if (directory.empty()) {
        picked.error = "no private storage to install the driver into";
        return picked;
    }
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    const std::optional<std::vector<std::string>> libraries = install_gpu_driver_zip(*document, staging.string());
    if (!libraries || libraries->empty()) {
        picked.error = libraries ? "not a driver package: no .so file in that .zip"
                                 : "Android would not let Yakumo read that file.";
        fs::remove_all(staging, ec);
        return picked;
    }
    display_name_cache() = {};
    fs::remove_all(directory, ec);
    fs::rename(staging, directory, ec);
    if (ec) {
        picked.error = "could not install the driver into the app's private storage";
        fs::remove_all(staging, ec);
        return picked;
    }
    picked.library = choose_main_library(*libraries, json_string_field(read_meta_json(directory), "libraryName"));
    return picked;
}

void clear_custom_gpu_driver() {
    display_name_cache() = {};
    std::error_code ec;
    fs::remove_all(driver_directory(), ec);
}

void begin_driver_trial() {
    std::ofstream(trial_marker()).put('\n');
}

void end_driver_trial() {
    std::error_code ec;
    fs::remove(trial_marker(), ec);
}

bool driver_trial_interrupted() {
    std::error_code ec;
    return fs::exists(trial_marker(), ec);
}

std::string driver_display_name(const std::string &library) {
    if (library.empty()) return {};
    DisplayNameCache &cache = display_name_cache();
    if (!cache.valid || cache.library != library) {
        const std::string name = json_string_field(read_meta_json(driver_directory()), "name");
        cache = {library, name.empty() ? library : name, true};
    }
    return cache.name;
}

void *open_custom_gpu_driver(const std::string &library, std::string &error) {
    // settings.ini can be edited by hand: only a plain file name may name the
    // driver, never a path that leads out of driver_directory().
    const fs::path name(library);
    if (name.empty() || name.filename() != name || library == "." || library == "..") {
        error = "the driver setting is not a file name; pick the driver again";
        return nullptr;
    }
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
