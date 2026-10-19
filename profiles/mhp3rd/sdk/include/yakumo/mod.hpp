// A thin C++ layer over yakumo/mod.h: the same services, with typed lookups
// and lambdas for callbacks. Header-only; it adds no ABI of its own.
#ifndef YAKUMO_MOD_HPP
#define YAKUMO_MOD_HPP

#include "yakumo/mod.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>

namespace yakumo {

class Mod {
public:
    Mod(const ymod_host *host, ymod_mod *mod) noexcept : host_(host), mod_(mod) {}

    [[nodiscard]] ymod_mod *handle() const noexcept { return mod_; }
    [[nodiscard]] const ymod_host *host() const noexcept { return host_; }

    // A service table, or null when the host has none at that version.
    template <typename Table> const Table *service(const char *name, std::uint32_t version) const {
        return static_cast<const Table *>(host_->service(mod_, name, version));
    }
    const ymod_memory *memory() const { return service<ymod_memory>(YMOD_MEMORY, 1u); }
    const ymod_hooks *hooks() const { return service<ymod_hooks>(YMOD_HOOKS, 1u); }
    const ymod_calls *calls() const { return service<ymod_calls>(YMOD_CALLS, 1u); }
    const ymod_events *events() const { return service<ymod_events>(YMOD_EVENTS, 1u); }
    const ymod_files *files() const { return service<ymod_files>(YMOD_FILES, 1u); }
    const ymod_settings *settings() const { return service<ymod_settings>(YMOD_SETTINGS, 1u); }
    const ymod_services *services() const { return service<ymod_services>(YMOD_SERVICES, 1u); }
    const ymod_symbols *symbols() const { return service<ymod_symbols>(YMOD_SYMBOLS, 1u); }
    const ymod_ui *ui() const { return service<ymod_ui>(YMOD_UI, 1u); }

    // A known function or data by name (sdk/symbols.txt), or nothing.
    [[nodiscard]] std::optional<ymod_symbol> symbol(const char *name) const {
        const ymod_symbols *table = symbols();
        ymod_symbol found{sizeof(ymod_symbol)};
        if (table == nullptr || table->find(name, &found) == 0u) return std::nullopt;
        return found;
    }
    // A function hook on a known function, by name: 0 when the name is
    // unknown or the hook fails.
    ymod_hook hook(const char *name, int kind, ymod_hook_fn callback, void *user = nullptr) const {
        const std::optional<ymod_symbol> found = symbol(name);
        const ymod_hooks *table = hooks();
        if (!found || found->kind != YMOD_SYMBOL_FUNCTION || table == nullptr) return 0u;
        return table->function(mod_, found->overlay, found->address, kind, callback, user);
    }

    void log(const std::string &message, int level = YMOD_LOG_INFO) const { host_->log(mod_, level, message.c_str()); }
    [[nodiscard]] std::string folder() const { return host_->folder(mod_); }

    // Guest memory as a value of type T, little-endian as on the PSP.
    template <typename T> std::optional<T> read(std::uint32_t address) const {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        const ymod_memory *table = memory();
        if (table == nullptr || table->read(address, &value, sizeof(T)) != 0) return std::nullopt;
        return value;
    }
    template <typename T> bool write(std::uint32_t address, const T &value) const {
        static_assert(std::is_trivially_copyable_v<T>);
        const ymod_memory *table = memory();
        return table != nullptr && table->write(address, &value, sizeof(T)) == 0;
    }

private:
    const ymod_host *host_;
    ymod_mod *mod_;
};

} // namespace yakumo

#endif // YAKUMO_MOD_HPP
