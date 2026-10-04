#include "hle/hle_common.hpp"
#include "kernel/kernel.hpp"
#include "psprecomp/common.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}
void memory_contracts() {
    psprecomp::Runtime runtime(64u * 1024u * 1024u);
    mhp3rd::Kernel kernel;
    kernel.install(runtime, 0u, 0x08804123u);
    const auto total = kernel.free_memory();
    check(total == mhp3rd::kUserMemoryEnd - 0x08805000u, "image end rounds up to page");
    check(static_cast<unsigned>(kernel.allocate_block("zero", 0, 0, 0)) == mhp3rd::error::kIllegalArgument,
        "zero allocation rejected");
    check(static_cast<unsigned>(kernel.allocate_block("bad", 5, 256, 0)) == mhp3rd::error::kIllegalMemblockType,
        "unknown partition type rejected");
    check(static_cast<unsigned>(kernel.allocate_block("bad", 3, 256, 384)) == mhp3rd::error::kIllegalArgument,
        "alignment must be a power of two");
    const auto low = kernel.allocate_block("low", 0, 1, 0);
    const auto high = kernel.allocate_block("high", 1, 257, 0);
    check(low > 0 && high > 0, "low/high allocations succeed");
    check(kernel.find_block(low)->address == 0x08805000u && kernel.find_block(low)->size == 256u,
        "low allocation rounds size");
    check(
        kernel.find_block(high)->address == mhp3rd::kUserMemoryEnd - 512u, "high allocation starts at upper boundary");
    const auto middle = kernel.allocate_block("middle", 2, 512, 0x0900017fu);
    check(middle > 0 && kernel.find_block(middle)->address == 0x09000100u, "fixed allocation rounds address down");
    check(static_cast<unsigned>(kernel.allocate_block("overlap", 2, 256, 0x09000100u)) == mhp3rd::error::kNoMemory,
        "overlapping fixed allocations fail");
    const auto aligned = kernel.allocate_block("aligned", 3, 256, 4096);
    const auto aligned_high = kernel.allocate_block("aligned-high", 4, 256, 4096);
    check(aligned > 0 && aligned_high > 0, "aligned allocations succeed");
    check((kernel.find_block(aligned)->address & 4095u) == 0 && (kernel.find_block(aligned_high)->address & 4095u) == 0,
        "both allocation directions honor alignment");
    for (auto uid : {middle, low, aligned_high, high, aligned})
        check(kernel.free_block(uid) == 0, "free succeeds in shuffled order");
    check(kernel.free_memory() == total, "all free fragments coalesce without losing bytes");
    check(kernel.find_block(low) == nullptr &&
            static_cast<unsigned>(kernel.free_block(low)) == mhp3rd::error::kIllegalMemblock,
        "double free rejected");
    const auto whole = kernel.allocate_block("whole", 0, total, 0);
    check(whole > 0 && kernel.free_memory() == 0, "coalesced range accepts entire region");
    check(static_cast<unsigned>(kernel.allocate_block("exhausted", 0, 256, 0)) == mhp3rd::error::kNoMemory,
        "exhaustion rejected");
    check(kernel.free_block(whole) == 0, "entire region can be returned");
}
class Fixture {
public:
    static constexpr std::uint32_t text = 0x08810000u;
    static constexpr std::uint32_t output = 0x08811000u;
    psprecomp::Runtime runtime{64u * 1024u * 1024u};
    mhp3rd::Kernel &kernel = mhp3rd::kernel();
    psprecomp::AllegrexContext &ctx = runtime.cpu();
    std::map<std::pair<std::string, std::string>, std::uint32_t> nids;
    Fixture() {
        kernel = mhp3rd::Kernel{};
        kernel.install(runtime, 0x08801000u, 0x08820000u);
        runtime.nids().load_csv(PSPRECOMP_TEST_NIDS_CSV);
        for (const auto &symbol : runtime.nids().all()) nids[{symbol.library, symbol.name}] = symbol.nid;
        mhp3rd::HleRegistrar hle(runtime);
        mhp3rd::register_threadman(hle);
        mhp3rd::register_sysmem(hle);
        mhp3rd::register_system(hle);
        kernel.start_loader_thread(ctx, 0x08820000u, 0);
        string(text, "contract");
    }
    ~Fixture() {
        kernel = mhp3rd::Kernel{};
        psprecomp::set_runtime_starvation_hook(nullptr, 0);
    }
    void string(std::uint32_t address, std::string_view value) {
        mhp3rd::write_cstring(runtime.memory(), address, value, 512);
    }
    std::uint32_t call(std::string library, std::string name, std::initializer_list<std::uint32_t> args = {}) {
        for (unsigned i = 0; i < 8; ++i) ctx.set_gpr(i < 4 ? i + 4 : i + 4, 0);
        unsigned i = 0;
        for (const auto value : args) ctx.set_gpr(i++ + 4, value);
        ctx.set_gpr(31, 0x08822000u);
        runtime.invoke_import(library, nids.at({library, name}), ctx);
        return ctx.gpr[2];
    }
    std::uint32_t thread(std::string name, std::initializer_list<std::uint32_t> args = {}) {
        return call("ThreadManForUser", std::move(name), args);
    }
    std::uint32_t helper(std::uint32_t priority = 0x30u) {
        const auto uid = kernel.create_thread("helper", 0x08824000u, priority, 0x1000u, 0, 0x08801000u);
        check(uid > 0 && kernel.start_thread(ctx, uid, 0, 0) == 0, "helper thread starts");
        return static_cast<std::uint32_t>(uid);
    }
};

void scheduler_contracts() {
    Fixture f;
    auto &k = f.kernel;
    auto &ctx = f.ctx;
    const auto loader = k.current_uid();
    check(ctx.gpr[28] == 0x08801000u && ctx.gpr[31] == mhp3rd::kThreadExitStub,
        "loader starts with GP and exit trampoline");
    check(f.runtime.memory().load32(ctx.gpr[26] + 0xC0) == static_cast<unsigned>(loader),
        "thread control block identifies current thread");
    for (auto entry : {0u, 0x08824001u})
        check(
            static_cast<unsigned>(k.create_thread("invalid", entry, 0x30, 4096, 0, 0)) == mhp3rd::error::kIllegalEntry,
            "bad entry rejected");
    for (auto priority : {0u, 128u})
        check(static_cast<unsigned>(k.create_thread("invalid", 0x08824000, priority, 4096, 0, 0)) ==
                mhp3rd::error::kIllegalPriority,
            "bad priority rejected");
    check(static_cast<unsigned>(k.create_thread("invalid", 0x08824000, 0x30, 511, 0, 0)) ==
            mhp3rd::error::kIllegalStackSize,
        "undersized stack rejected");
    check(
        static_cast<unsigned>(k.start_thread(ctx, -1, 0, 0)) == mhp3rd::error::kUnknownThid, "unknown start rejected");
    const auto high = k.create_thread("high", 0x08824000, 0x10, 4096, 0, 0x1234);
    f.string(Fixture::output, "arguments");
    check(k.start_thread(ctx, high, 10, Fixture::output) == 0, "start copies arguments");
    const auto *t = k.find_thread(high);
    check(t->context.gpr[4] == 10 && mhp3rd::read_cstring(f.runtime.memory(), t->context.gpr[5]) == "arguments",
        "thread argument bytes and size retained");
    check(static_cast<unsigned>(k.start_thread(ctx, high, 0, 0)) == mhp3rd::error::kNotDormant,
        "running thread cannot start twice");
    check(
        static_cast<unsigned>(k.delete_thread(high)) == mhp3rd::error::kNotDormant, "active thread cannot be deleted");
    ctx.set_gpr(31, 0x08823000);
    k.finish(ctx, 42);
    check(k.current_uid() == high && ctx.pc == 0x08824000, "higher priority thread preempts on import completion");
    check(k.find_thread(loader)->context.pc == 0x08823000 && k.find_thread(loader)->context.gpr[2] == 42,
        "preemption saves return address and result");
    check(static_cast<unsigned>(k.terminate_thread(ctx, high, false)) == mhp3rd::error::kIllegalThid,
        "cannot terminate current thread");
    k.delay_current(ctx, 2500, 77);
    check(k.current_uid() == loader && k.find_thread(high)->status == mhp3rd::ThreadStatus::Waiting,
        "delay yields to ready loader");
    k.delay_current(ctx, 3000);
    check(k.now_us() == 2500 && k.current_uid() == high && ctx.gpr[2] == 77,
        "earliest deadline advances virtual clock and resumes result");
    k.exit_current_thread(ctx, 123, false);
    check(k.find_thread(high)->exit_status == 123 && k.current_uid() == loader && k.now_us() == 3000,
        "exit status retained and next timed thread resumes");
    check(k.delete_thread(high) == 0 && k.find_thread(high) == nullptr,
        "dormant thread deletion removes stack and identity");
    check(static_cast<unsigned>(k.delete_thread(high)) == mhp3rd::error::kUnknownThid, "repeated thread delete fails");
    check(static_cast<unsigned>(k.change_priority(ctx, -1, 1)) == mhp3rd::error::kUnknownThid,
        "unknown priority target rejected");
    check(static_cast<unsigned>(k.change_priority(ctx, 0, 128)) == mhp3rd::error::kIllegalPriority,
        "out of range changed priority rejected");
    check(k.change_priority(ctx, 0, 0) == 0, "zero priority inherits current priority");
    check(f.thread("sceKernelGetThreadId") == static_cast<unsigned>(loader), "thread HLE returns live identity");
    check(f.thread("sceKernelGetThreadCurrentPriority") == 0x20, "thread HLE returns loader priority");
    check(f.thread("sceKernelSuspendDispatchThread") == 1 && !k.dispatch_enabled(),
        "dispatch suspension returns previous flag");
    check(f.thread("sceKernelSuspendDispatchThread") == 0, "nested suspension reports disabled state");
    check(f.thread("sceKernelResumeDispatchThread", {1}) == 0 && k.dispatch_enabled(),
        "dispatch resume restores scheduler");
    check(f.thread("sceKernelWakeupThread", {static_cast<unsigned>(loader)}) == 0, "self wake records a wake token");
    check(f.thread("sceKernelSleepThread") == 0 && k.current_uid() == loader,
        "sleep consumes wake token without blocking");
    check(f.thread("sceKernelWakeupThread", {static_cast<unsigned>(loader)}) == 0 &&
            f.thread("sceKernelSleepThreadCB") == 0,
        "callback-aware sleep consumes wake token");
    check(f.thread("sceKernelWakeupThread", {0xFFFFFFFFu}) == mhp3rd::error::kUnknownThid,
        "wake unknown thread rejected");
    check(f.thread("sceKernelDelayThread", {50}) == 0 && k.now_us() == 3050, "delay HLE charges exact virtual time");
    check(
        f.thread("sceKernelDelayThreadCB", {50}) == 0 && k.now_us() == 3100, "callback delay has same clock semantics");
    check(f.thread("sceKernelGetSystemTime", {Fixture::output}) == 0 &&
            f.runtime.memory().load32(Fixture::output) == 3100,
        "time output matches scheduler clock");
    check(f.thread("sceKernelGetSystemTimeWide") == 3100 && ctx.gpr[3] == 0, "wide system time returns register pair");
    check(f.thread("sceKernelGetSystemTimeLow") == 3100, "low system time matches wide time");
    check(f.thread("sceKernelSysClock2USecWide", {2000003, 0, Fixture::output, Fixture::output + 4}) == 0,
        "clock conversion succeeds");
    check(f.runtime.memory().load32(Fixture::output) == 2 && f.runtime.memory().load32(Fixture::output + 4) == 3,
        "clock conversion splits seconds and microseconds");
    check(k.describe_threads().find("module_start") != std::string::npos, "diagnostics identify live thread");
}

void semaphore_contracts() {
    Fixture f;
    for (auto args : {std::array<unsigned, 2>{0, 0}, {2, 1}, {0xFFFFFFFFu, 1}})
        check(f.thread("sceKernelCreateSema", {Fixture::text, 0, args[0], args[1]}) == mhp3rd::error::kIllegalCount,
            "invalid semaphore counts rejected");
    const auto uid = f.thread("sceKernelCreateSema", {Fixture::text, 0, 1, 3});
    check(static_cast<int>(uid) > 0, "semaphore creation succeeds");
    check(f.thread("sceKernelPollSema", {uid, 1}) == 0 && f.kernel.semaphores.at(uid).count == 0,
        "poll consumes available count");
    check(f.thread("sceKernelPollSema", {uid, 1}) == mhp3rd::error::kSemaZero, "empty poll does not wait");
    check(f.thread("sceKernelPollSema", {uid, 0}) == mhp3rd::error::kIllegalCount, "zero poll count rejected");
    check(f.thread("sceKernelSignalSema", {uid, 4}) == mhp3rd::error::kSemaOverflow, "overflow leaves count unchanged");
    check(f.kernel.semaphores.at(uid).count == 0, "overflow preserves state");
    check(f.thread("sceKernelSignalSema", {uid, 2}) == 0 && f.thread("sceKernelWaitSema", {uid, 1, 0}) == 0,
        "immediate wait consumes signal");
    check(f.thread("sceKernelWaitSema", {uid, 0, 0}) == mhp3rd::error::kIllegalCount &&
            f.thread("sceKernelWaitSema", {uid, 4, 0}) == mhp3rd::error::kIllegalCount,
        "wait validates requested count");
    f.runtime.memory().store32(Fixture::output, 250);
    const auto start = f.kernel.now_us();
    check(f.thread("sceKernelWaitSema", {uid, 2, Fixture::output}) == mhp3rd::error::kWaitTimeout,
        "semaphore wait times out deterministically");
    check(f.kernel.now_us() == start + 250 && f.runtime.memory().load32(Fixture::output) == 0 &&
            f.kernel.semaphores.at(uid).waiters.empty(),
        "timeout zeros remaining time and removes waiter");
    const auto loader = f.kernel.current_uid();
    const auto helper = f.helper();
    f.thread("sceKernelWaitSema", {uid, 2, 0});
    check(f.kernel.current_uid() == static_cast<int>(helper), "blocked semaphore switches to helper");
    check(f.thread("sceKernelSignalSema", {uid, 1}) == 0 && f.kernel.current_uid() == loader,
        "signal wakes and preempts to higher priority waiter");
    check(f.kernel.semaphores.at(uid).count == 0, "wake consumes full requested count");
    f.thread("sceKernelWaitSema", {uid, 1, 0});
    check(f.thread("sceKernelDeleteSema", {uid}) == mhp3rd::error::kWaitDelete && f.kernel.current_uid() == loader,
        "delete wakes waiter with deletion result");
    for (const auto *name : {"sceKernelDeleteSema", "sceKernelSignalSema", "sceKernelWaitSema", "sceKernelPollSema"})
        check(f.thread(name, {uid, 1, 0}) == mhp3rd::error::kUnknownSemid, "deleted semaphore returns unknown UID");
}

void event_flag_contracts() {
    Fixture f;
    const auto uid = f.thread("sceKernelCreateEventFlag", {Fixture::text, 0, 0x3});
    check(f.thread("sceKernelPollEventFlag", {uid, 3, 0, Fixture::output}) == 0 &&
            f.runtime.memory().load32(Fixture::output) == 3,
        "AND match returns full pattern");
    check(f.thread("sceKernelPollEventFlag", {uid, 6, 0, Fixture::output}) == mhp3rd::error::kEvfCond &&
            f.runtime.memory().load32(Fixture::output) == 3,
        "failed poll returns unchanged pattern");
    check(f.thread("sceKernelPollEventFlag", {uid, 6, 1, 0}) == 0, "OR mode succeeds on any requested bit");
    for (auto mode : {2u, 0x30u})
        check(f.thread("sceKernelPollEventFlag", {uid, 1, mode, 0}) == mhp3rd::error::kIllegalMode,
            "invalid clear/match mode rejected");
    check(f.thread("sceKernelPollEventFlag", {uid, 0, 0, 0}) == mhp3rd::error::kEvfIllegalPattern,
        "zero pattern rejected");
    check(f.thread("sceKernelPollEventFlag", {uid, 1, 0x20, 0}) == 0 && f.kernel.event_flags.at(uid).pattern == 2,
        "clear requested bits preserves unrelated bits");
    check(f.thread("sceKernelPollEventFlag", {uid, 2, 0x10, 0}) == 0 && f.kernel.event_flags.at(uid).pattern == 0,
        "clear all erases full matched pattern");
    check(f.thread("sceKernelSetEventFlag", {uid, 7}) == 0 && f.thread("sceKernelClearEventFlag", {uid, 6}) == 0 &&
            f.kernel.event_flags.at(uid).pattern == 6,
        "set ORs and clear ANDs pattern");
    check(f.thread("sceKernelWaitEventFlag", {uid, 2, 0, 0, 0}) == 0, "matching wait completes immediately");
    f.runtime.memory().store32(Fixture::output, 50);
    check(f.thread("sceKernelWaitEventFlag", {uid, 8, 0, 0, Fixture::output}) == mhp3rd::error::kWaitTimeout &&
            f.kernel.event_flags.at(uid).waiters.empty(),
        "event timeout removes queue entry");
    const auto loader = f.kernel.current_uid();
    f.helper();
    f.thread("sceKernelWaitEventFlag", {uid, 8, 0x20, Fixture::output, 0});
    check(f.thread("sceKernelWaitEventFlag", {uid, 8, 0, 0, 0}) == mhp3rd::error::kEvfMulti,
        "single-waiter flag rejects second blocking waiter");
    f.thread("sceKernelSetEventFlag", {uid, 8});
    check(f.kernel.current_uid() == loader && f.ctx.gpr[2] == 0 && f.runtime.memory().load32(Fixture::output) == 14 &&
            f.kernel.event_flags.at(uid).pattern == 6,
        "signal delivers output then clears matching bits");
    f.thread("sceKernelWaitEventFlag", {uid, 8, 0, 0, 0});
    f.thread("sceKernelDeleteEventFlag", {uid});
    check(f.kernel.current_uid() == loader && f.ctx.gpr[2] == mhp3rd::error::kWaitDelete,
        "event deletion cancels waiter");
    for (const auto *name :
        {"sceKernelDeleteEventFlag", "sceKernelSetEventFlag", "sceKernelClearEventFlag", "sceKernelPollEventFlag"})
        check(f.thread(name, {uid, 1}) == mhp3rd::error::kUnknownEvfid, "deleted event flag rejects operations");
}

void mutex_contracts() {
    Fixture f;
    const auto uid = f.thread("sceKernelCreateMutex", {Fixture::text, 0, 0});
    check(
        f.thread("sceKernelLockMutex", {uid, 0, 0}) == mhp3rd::error::kIllegalCount, "mutex zero lock count rejected");
    check(f.thread("sceKernelLockMutex", {uid, 1, 0}) == 0, "unowned mutex locks");
    check(f.thread("sceKernelLockMutex", {uid, 1, 0}) == mhp3rd::error::kMutexRecursiveNotAllowed,
        "nonrecursive re-lock rejected");
    check(f.thread("sceKernelUnlockMutex", {uid, 0}) == mhp3rd::error::kIllegalCount, "zero unlock count rejected");
    check(f.thread("sceKernelUnlockMutex", {uid, 2}) == mhp3rd::error::kMutexUnlockUnderflow,
        "unlock underflow preserves ownership");
    check(f.thread("sceKernelUnlockMutex", {uid, 1}) == 0 &&
            f.thread("sceKernelUnlockMutex", {uid, 1}) == mhp3rd::error::kMutexUnlocked,
        "balanced unlock clears ownership");
    const auto recursive = f.thread("sceKernelCreateMutex", {Fixture::text, 0x200, 2});
    check(f.thread("sceKernelLockMutex", {recursive, 3, 0}) == 0 && f.kernel.mutexes.at(recursive).lock_count == 5,
        "recursive mutex adds count");
    check(f.thread("sceKernelUnlockMutex", {recursive, 5}) == 0, "recursive mutex releases all references");
    const auto loader = f.kernel.current_uid();
    const auto helper = f.helper();
    f.thread("sceKernelDelayThread", {100});
    check(f.kernel.current_uid() == static_cast<int>(helper), "loader delay yields to helper");
    check(f.thread("sceKernelLockMutex", {uid, 1, 0}) == 0, "helper owns mutex");
    f.kernel.on_starvation(f.ctx);
    check(f.kernel.current_uid() == loader, "elapsed loader deadline preempts helper");
    f.runtime.memory().store32(Fixture::output, 100);
    f.thread("sceKernelLockMutex", {uid, 1, Fixture::output});
    check(f.kernel.current_uid() == static_cast<int>(helper), "contended lock yields to owner");
    f.thread("sceKernelUnlockMutex", {uid, 1});
    check(f.kernel.current_uid() == loader && f.kernel.mutexes.at(uid).owner == loader &&
            f.runtime.memory().load32(Fixture::output) == 100,
        "unlock transfers ownership and retains remaining timeout");
    f.thread("sceKernelUnlockMutex", {uid, 1});
    check(f.thread("sceKernelDeleteMutex", {uid}) == 0 && f.thread("sceKernelDeleteMutex", {recursive}) == 0,
        "mutex deletion succeeds");
    for (const auto *name : {"sceKernelLockMutex", "sceKernelUnlockMutex", "sceKernelDeleteMutex"})
        check(f.thread(name, {uid, 1, 0}) == mhp3rd::error::kMutexNotFound, "deleted mutex rejects operations");
}

}
int main() {
    try {
        memory_contracts();
        scheduler_contracts();
        semaphore_contracts();
        event_flag_contracts();
        mutex_contracts();
        std::cout << "kernel/HLE contracts passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
