#include "mods/mod_native_internal.hpp"

#include "mods/mhp3rd_symbols.hpp"
#include "mods/mod_ini.hpp"

#include "kernel/kernel.hpp"
#include "overlays.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/function_observer.hpp"

#include <algorithm>
#include <array>
#include <utility>

// Hooks: where mods take over a game function's entry or run before one of
// its instructions, and guest calls from a replacement.
namespace mhp3rd::mods::native {
namespace {

const std::vector<Hook> *hooks_at(std::uint32_t address) {
    State &s = state();
    const auto found = s.sites.find(address);
    return found != s.sites.end() ? &found->second.hooks : nullptr;
}

// Callbacks may add and remove hooks, so they run from a copy of the list.
void run_after(std::uint32_t address, AllegrexContext &ctx) {
    if (const std::vector<Hook> *hooks = hooks_at(address)) {
        const std::vector<Hook> calling = *hooks;
        for (const Hook &hook : calling)
            if (hook.kind == YMOD_HOOK_AFTER) (void)hook.callback(as_cpu(ctx), hook.user);
    }
}

// Host features registered over the game's functions (their names start with
// "mhp3rd_"), hooked or not: a hook installed later wraps the feature.
struct HostFunction {
    Runtime::RecompiledFunction function{};
    std::string name;
};
std::unordered_map<std::uint32_t, HostFunction> &host_functions() {
    static std::unordered_map<std::uint32_t, HostFunction> functions;
    return functions;
}

// Every registration passes here. Something other than a hook taking a hooked
// address over takes the hook down: the next frame puts it back on top.
void observe(Runtime &, std::uint32_t address, Runtime::RecompiledFunction function, const std::string &name) {
    if (name == "mod_hook") return;
    if (name.starts_with("mhp3rd_"))
        host_functions()[address] = {function, name};
    else if (!host_functions().empty())
        host_functions().erase(address);
    State &s = state();
    if (s.sites.empty()) return;
    if (const auto site = s.sites.find(address); site != s.sites.end()) site->second.installed = false;
}
[[maybe_unused]] const bool kObserving = (psprecomp::set_function_observer(&observe), true);

// Registered at every hooked function entry.
void hook_entry(Runtime &runtime, AllegrexContext &ctx) {
    State &s = state();
    const std::uint32_t address = ctx.pc;
    const auto found = s.sites.find(address);
    if (found == s.sites.end() || found->second.original == nullptr) {
        runtime.stop("a mod's hook at " + psprecomp::hex32(address) + " has no game code behind it");
        return;
    }
    // What runs behind the hooks, taken before they run: a callback may
    // remove the last hook, and with it the site.
    Site &site = found->second;
    if (site.host == nullptr && !psprecomp::jit_detached_current(runtime, site.original)) {
        // The game's code there changed since it was compiled.
        site.original = psprecomp::jit_compile_detached(runtime, address);
        if (site.original == nullptr) {
            runtime.stop("the game's code under a mod's hook at " + psprecomp::hex32(address) + " cannot be compiled");
            return;
        }
    }
    const psprecomp::JitFunction original = site.original;
    const Runtime::RecompiledFunction host = site.host;
    const std::vector<Hook> calling = site.hooks;
    bool run_original = true;
    bool after = false;
    for (const Hook &hook : calling) {
        if (hook.kind == YMOD_HOOK_BEFORE) (void)hook.callback(as_cpu(ctx), hook.user);
        if (hook.kind == YMOD_HOOK_AFTER) after = true;
    }
    for (const Hook &hook : calling) {
        if (hook.kind != YMOD_HOOK_REPLACE) continue;
        s.call_pending = false;
        const bool replacing = std::exchange(s.replacing, true);
        const int result = hook.callback(as_cpu(ctx), hook.user);
        s.replacing = replacing;
        // The callback called guest code: it runs now and returns to the caller.
        if (s.call_pending) return;
        if (result == YMOD_RETURN) run_original = false;
        break; // the first replacement decides
    }
    // A callback that sent the guest elsewhere wins.
    if (ctx.pc != address) return;
    if (!run_original) {
        if (after) run_after(address, ctx);
        ctx.pc = ctx.gpr[31];
        return;
    }
    if (after) {
        s.returns[psprecomp::runtime_thread_uid()].push_back({ctx.gpr[31], address});
        ctx.gpr[31] = s.trampoline;
    }
    if (host != nullptr)
        host(runtime, ctx);
    else
        original(runtime, ctx);
}

bool overlay_present(const std::string &overlay, std::uint32_t address) {
    if (overlay.empty()) return true;
    const std::optional<LoadedOverlay> loaded = overlay_at(state().runtime->memory(), address);
    return loaded && iequals(loaded->name, overlay);
}

// Takes the hooks off their address. What was there before them takes it
// back: the host feature they wrapped, or nothing, and the game's own code is
// compiled again from memory.
void uninstall(std::uint32_t address, Site &site) {
    Runtime &runtime = *state().runtime;
    if (site.installed) {
        runtime.unregister_functions(address, address + 1u);
        psprecomp::jit_invalidate(runtime, address, address + 8u);
        if (site.host != nullptr) runtime.register_function(address, site.host, site.host_name);
    }
    site.installed = false;
}

bool install_site(std::uint32_t address, Site &site) {
    State &s = state();
    Runtime &runtime = *s.runtime;
    if (!site.overlay.empty()) {
        const std::optional<LoadedOverlay> loaded = overlay_at(runtime.memory(), address);
        if (!loaded || !iequals(loaded->name, site.overlay)) return false;
        (void)install_overlay_at(runtime, loaded->base);
    }
    if (!symbols::matches(address, site.overlay, runtime.memory())) {
        if (!site.refused)
            report(nullptr,
                "a hook at " + psprecomp::hex32(address) +
                    " is not installed: the game's code there is not the function the symbol table knows",
                true);
        site.refused = true;
        return false;
    }
    site.refused = false;
    const auto host = host_functions().find(address);
    site.host = host != host_functions().end() ? host->second.function : nullptr;
    site.host_name = host != host_functions().end() ? host->second.name : std::string();
    // Registering drops compiled code there; the game's own first
    // instructions are compiled afterwards, for the hook to run.
    runtime.register_function(address, &hook_entry, "mod_hook");
    site.installed = true;
    site.original = psprecomp::jit_compile_detached(runtime, address);
    if (site.original == nullptr) uninstall(address, site);
    return site.installed;
}

ymod_hook add_hook(ymod_mod *mod, const char *overlay, std::uint32_t address, int kind, ymod_hook_fn callback,
    void *user, bool instruction) {
    State &s = state();
    if (s.runtime == nullptr || callback == nullptr || (address & 3u) != 0u || (mod != nullptr && mod->failed))
        return 0u;
    if (!instruction && kind != YMOD_HOOK_BEFORE && kind != YMOD_HOOK_AFTER && kind != YMOD_HOOK_REPLACE) return 0u;
    const ymod_hook id = s.next_hook++;
    const Hook hook{id, mod, kind, callback, user};
    const std::string overlay_name = overlay != nullptr ? overlay : "";
    if (instruction) {
        InstructionSite &site = s.instructions[address];
        site.overlay = overlay_name;
        site.hooks.push_back(hook);
    } else {
        Site &site = s.sites[address];
        site.overlay = overlay_name;
        site.hooks.push_back(hook);
    }
    s.hook_sites[id] = {address, instruction};
    reconcile();
    return id;
}

ymod_hook hooks_function(
    ymod_mod *mod, const char *overlay, std::uint32_t address, int kind, ymod_hook_fn callback, void *user) {
    return add_hook(mod, overlay, address, kind, callback, user, false);
}

ymod_hook hooks_instruction(
    ymod_mod *mod, const char *overlay, std::uint32_t address, ymod_hook_fn callback, void *user) {
    return add_hook(mod, overlay, address, YMOD_HOOK_BEFORE, callback, user, true);
}

int calls_call(
    ymod_cpu *cpu, std::uint32_t address, const std::uint32_t arguments[4], ymod_call_done_fn done, void *user) {
    // A call returns to the hooked function's caller, so only a replacement,
    // which decides what the function does, can make one.
    if (cpu == nullptr || (address & 3u) != 0u || !state().replacing) return -1;
    auto &ctx = *reinterpret_cast<AllegrexContext *>(cpu);
    const std::array<std::uint32_t, 4> args{arguments != nullptr ? arguments[0] : 0u,
        arguments != nullptr ? arguments[1] : 0u, arguments != nullptr ? arguments[2] : 0u,
        arguments != nullptr ? arguments[3] : 0u};
    kernel().call_guest(ctx, address, args, [done, user](AllegrexContext &back, std::uint32_t result) {
        if (done != nullptr) done(as_cpu(back), result, user);
    });
    state().call_pending = true;
    return 0;
}

} // namespace

// Where a function with AFTER hooks returns to.
void hook_return(Runtime &runtime, AllegrexContext &ctx) {
    State &s = state();
    std::vector<Return> &stack = s.returns[psprecomp::runtime_thread_uid()];
    if (stack.empty()) {
        runtime.stop("guest code returned to a mod's hook with no call in progress");
        return;
    }
    const Return back = stack.back();
    stack.pop_back();
    ctx.gpr[31] = back.ra;
    ctx.pc = back.ra;
    run_after(back.address, ctx);
}

void instruction_dispatch(Runtime &, AllegrexContext &ctx, std::uint32_t address) {
    State &s = state();
    const auto found = s.instructions.find(address);
    if (found == s.instructions.end()) return;
    const std::vector<Hook> calling = found->second.hooks;
    for (const Hook &hook : calling) (void)hook.callback(as_cpu(ctx), hook.user);
}

// Overlay hooks follow their overlay: in place while it is loaded. And a host
// feature that takes over a hooked function, or gives it back, does it under
// the hooks: they go back on top, around whatever is there now.
void reconcile() {
    State &s = state();
    if (s.runtime == nullptr) return;
    Runtime &runtime = *s.runtime;
    for (auto &[address, site] : s.sites) {
        if (site.hooks.empty()) continue;
        const bool present = overlay_present(site.overlay, address);
        if (present && (!site.installed || !runtime.has_function(address))) {
            site.installed = false;
            (void)install_site(address, site);
        } else if (!present && site.installed) {
            uninstall(address, site);
        }
    }
    for (auto &[address, site] : s.instructions) {
        const bool present = !site.hooks.empty() && overlay_present(site.overlay, address);
        if (present && !site.installed) {
            // The overlay's recompiled code goes in first, or installing it
            // later would cover the unit the hook takes out of it.
            if (!site.overlay.empty())
                if (const std::optional<LoadedOverlay> loaded = overlay_at(runtime.memory(), address))
                    (void)install_overlay_at(runtime, loaded->base);
            psprecomp::jit_hook_instruction(runtime, address);
            site.installed = true;
        } else if (!present && site.installed) {
            psprecomp::jit_unhook_instruction(runtime, address);
            site.installed = false;
        }
    }
}

void hooks_remove(ymod_mod *, ymod_hook hook) {
    State &s = state();
    const auto found = s.hook_sites.find(hook);
    if (found == s.hook_sites.end()) return;
    const auto [address, instruction] = found->second;
    s.hook_sites.erase(found);
    const auto drop = [hook](std::vector<Hook> &hooks) {
        std::erase_if(hooks, [hook](const Hook &h) { return h.id == hook; });
    };
    if (instruction) {
        drop(s.instructions[address].hooks);
    } else {
        Site &site = s.sites[address];
        drop(site.hooks);
        // The last hook gone: the game's own code again, or the host feature.
        if (site.hooks.empty()) uninstall(address, site);
    }
    reconcile();
}

const ymod_hooks kHooks{1u, sizeof(ymod_hooks), &hooks_function, &hooks_instruction, &hooks_remove};

const ymod_calls kCalls{1u, sizeof(ymod_calls), &calls_call};

} // namespace mhp3rd::mods::native
