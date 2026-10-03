// Pure selector tests have no disc, mod session or user's configuration.
#include "mods/mhp3rd_mods.hpp"
#include "install/user_data.hpp"
#include "kernel/kernel.hpp"
#include "psprecomp/common.hpp"
#include "settings/settings.hpp"

#include <filesystem>

namespace mhp3rd::mods {
ModSession *session() { return nullptr; }
}
namespace mhp3rd {
Kernel &kernel() {
    throw psprecomp::Error("Pure transmog selector tests must not execute native refresh scheduling.");
}
void Kernel::call_guest(AllegrexContext &, std::uint32_t, const std::array<std::uint32_t, 4> &, GuestCallReturn) {
    throw psprecomp::Error("Pure transmog selector tests must not call guest refresh routines.");
}
void Kernel::delay_guest_callback(AllegrexContext &, std::uint64_t, GuestCallReturn) {
    throw psprecomp::Error("Pure transmog selector tests must not schedule a guest refresh continuation.");
}
Thread *Kernel::find_thread(SceUID) noexcept { return nullptr; }
}
namespace mhp3rd::install {
namespace {
std::filesystem::path test_data_directory = std::filesystem::temp_directory_path() / "yakumo-transmog-selector-tests";
}
void set_transmog_test_data_directory(std::filesystem::path path) {
    test_data_directory = std::move(path);
}
std::filesystem::path user_data_directory() {
    return test_data_directory;
}
}

namespace mhp3rd::settings {
Settings &current() {
    static Settings value;
    return value;
}
}
