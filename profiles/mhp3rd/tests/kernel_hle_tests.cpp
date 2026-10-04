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
void interrupt_and_host_wait_contracts() {
    Fixture f;
    auto &k = f.kernel;
    auto &ctx = f.ctx;
    check(f.call("Kernel_Library", "sceKernelCpuSuspendIntr") == 1 && !k.interrupts_enabled(),
        "mask interrupts returns prior state");
    check(f.call("Kernel_Library", "sceKernelCpuSuspendIntr") == 0, "nested mask reports disabled");
    check(f.call("Kernel_Library", "sceKernelCpuResumeIntr", {1}) == 0 && k.interrupts_enabled(),
        "resume restores interrupts");
    check(f.call("Kernel_Library", "sceKernelMemset", {Fixture::output, 0x1ab, 8}) == Fixture::output &&
            f.runtime.memory().load32(Fixture::output) == 0xababababu,
        "memset truncates value and returns destination");
    const auto cb = f.thread("sceKernelCreateCallback", {Fixture::text, 0x08826000, 0x7654});
    k.notify_callback(-1, 1);
    k.notify_callback(cb, 7);
    k.notify_callback(cb, 9);
    check(k.deliver_callbacks() && !k.deliver_callbacks(), "notifications queue once and clear pending state");
    ctx.set_gpr(16, 0x12345678);
    ctx.set_gpr(31, 0x08827000);
    k.finish(ctx, 77);
    check(k.in_interrupt() && ctx.pc == 0x08826000 && ctx.gpr[4] == 2 && ctx.gpr[5] == 9 && ctx.gpr[6] == 0x7654,
        "callback receives count, latest notification and common data");
    check(ctx.gpr[29] == mhp3rd::kInterruptStackTop - 0x40 && ctx.gpr[31] == mhp3rd::kInterruptReturnStub,
        "interrupt uses reserved stack and trampoline");
    check(!k.deliver_callbacks(), "nested callback delivery suppressed");
    k.delay_current(ctx, 1);
    check(ctx.gpr[2] == mhp3rd::error::kCanNotWait, "interrupt handler cannot block");
    k.interrupt_return_stub(ctx);
    check(!k.in_interrupt() && ctx.gpr[16] == 0x12345678 && ctx.gpr[2] == 77 && ctx.pc == 0x08827000,
        "interrupt restores saved registers and import result");
    check(f.thread("sceKernelDeleteCallback", {cb}) == 0 &&
            f.thread("sceKernelDeleteCallback", {cb}) == mhp3rd::error::kUnknownCbid,
        "callback deletion validates identity");
    check(f.call("InterruptManager", "sceKernelEnableSubIntr", {30, 4}) == mhp3rd::error::kIllegalArgument,
        "missing interrupt cannot enable");
    check(f.call("InterruptManager", "sceKernelRegisterSubIntrHandler", {30, 4, 0x08828000, 123}) == 0 &&
            f.call("InterruptManager", "sceKernelEnableSubIntr", {30, 4}) == 0,
        "registered subinterrupt enables");
    unsigned vblanks = 0;
    k.add_vblank_hook([&] { ++vblanks; });
    k.on_vblank();
    k.finish(ctx, 0);
    check(k.in_interrupt() && ctx.pc == 0x08828000 && ctx.gpr[4] == 4 && ctx.gpr[5] == 123 && vblanks == 1,
        "vblank delivers guest handler and host hook");
    k.interrupt_return_stub(ctx);
    check(f.call("InterruptManager", "sceKernelReleaseSubIntrHandler", {30, 4}) == 0 &&
            f.call("InterruptManager", "sceKernelReleaseSubIntrHandler", {30, 4}) == mhp3rd::error::kIllegalArgument,
        "interrupt release rejects repeated delete");
    unsigned calls = 0;
    k.wait_host(ctx, std::nullopt, [&](bool timeout) -> std::optional<unsigned> {
        check(!timeout, "immediate host completion is not timeout");
        ++calls;
        return 17;
    });
    check(ctx.gpr[2] == 17 && calls == 1, "satisfied host wait completes without switching");
    calls = 0;
    k.wait_host(ctx, 0, [&](bool timeout) -> std::optional<unsigned> {
        ++calls;
        return timeout ? std::optional<unsigned>(19) : std::nullopt;
    });
    check(ctx.gpr[2] == 19 && calls == 2, "zero timeout performs final poll");
    const auto start = k.now_us();
    k.wait_host(ctx, 2000,
        [](bool timeout) -> std::optional<unsigned> { return timeout ? std::optional<unsigned>(23) : std::nullopt; });
    check(ctx.gpr[2] == 23 && k.now_us() == start + 2000, "host deadline advances clock and returns final poll value");
    k.wait_host(ctx, 100, [](bool) -> std::optional<unsigned> { return std::nullopt; });
    check(ctx.gpr[2] == mhp3rd::error::kWaitTimeout, "empty final poll falls back to kernel timeout");
    bool returned = false;
    ctx.set_gpr(31, 0x08829000);
    k.call_guest(ctx, 0x0882a000, {1, 2, 3, 4}, [&](auto &context, unsigned value) {
        returned = true;
        check(value == 33 && context.pc == 0x08829000 && context.gpr[31] == 0x08829000,
            "guest completion restores import return address");
    });
    check(ctx.pc == 0x0882a000 && ctx.gpr[4] == 1 && ctx.gpr[7] == 4 && ctx.gpr[31] == mhp3rd::kGuestCallReturnStub,
        "guest call receives argument registers and trampoline");
    ctx.set_gpr(2, 33);
    k.guest_call_return_stub(ctx);
    check(returned, "guest return invokes completion");
    k.guest_call_return_stub(ctx);
    check(f.runtime.stopped() && f.runtime.stop_reason().find("made none") != std::string::npos,
        "unexpected guest return stops with diagnostic");
}
void vtimer_contracts() {
    Fixture f;
    const auto uid = f.thread("sceKernelCreateVTimer", {Fixture::text});
    check(f.thread("sceKernelStartVTimer", {uid}) == 0 && f.thread("sceKernelStartVTimer", {uid}) == 1,
        "vtimer starts once");
    check(f.thread("sceKernelStartVTimer", {0xffffffffu}) == mhp3rd::error::kUnknownVtid &&
            f.thread("sceKernelSetVTimerHandlerWide", {0xffffffffu}) == mhp3rd::error::kUnknownVtid,
        "timer operations validate UID");
    check(f.thread("sceKernelSetVTimerHandlerWide", {uid, 0, 500, 0, 0x0882b000, 0x5432}) == 0,
        "timer sets schedule and common pointer");
    f.kernel.on_starvation(f.ctx);
    check(f.kernel.in_interrupt() && f.ctx.pc == 0x0882b000 && f.ctx.gpr[4] == uid && f.ctx.gpr[7] == 0x5432,
        "elapsed timer dispatches handler");
    check(f.runtime.memory().load32(f.ctx.gpr[5]) == 500 && f.runtime.memory().load32(f.ctx.gpr[6]) == 1000,
        "timer passes scheduled and current clocks");
    f.ctx.set_gpr(2, 2000);
    f.kernel.interrupt_return_stub(f.ctx);
    check(f.kernel.vtimers.at(uid).schedule_us == 2500, "timer rearms relative to original schedule");
    f.kernel.on_starvation(f.ctx);
    check(!f.kernel.in_interrupt(), "timer does not fire early");
    f.kernel.on_starvation(f.ctx);
    check(f.kernel.in_interrupt(), "rearmed timer fires at next schedule");
    f.ctx.set_gpr(2, 0);
    f.kernel.interrupt_return_stub(f.ctx);
    check(f.kernel.vtimers.at(uid).handler == 0, "zero handler result disarms timer");
    mhp3rd::VTimer stopped{};
    stopped.accumulated_us = 17;
    check(f.kernel.vtimer_value(stopped) == 17, "stopped timer retains accumulated clock");
}
void sysmem_contracts() {
    Fixture f;
    auto call = [&](const char *name, std::initializer_list<unsigned> args = {}) {
        return f.call("SysMemUserForUser", name, args);
    };
    const auto uid = call("sceKernelAllocPartitionMemory", {2, Fixture::text, 0, 512, 0});
    check(static_cast<int>(uid) > 0, "partition allocation returns UID");
    const auto address = call("sceKernelGetBlockHeadAddr", {uid});
    check(
        address != 0 && call("sceKernelGetBlockHeadAddr", {0xffffffffu}) == 0, "head address resolves valid UID only");
    check(call("sceKernelGetMemoryBlockAddr", {uid, Fixture::output}) == 0 &&
            f.runtime.memory().load32(Fixture::output) == address,
        "block address writes output");
    check(call("sceKernelGetMemoryBlockAddr", {uid, 0}) == 0 &&
            call("sceKernelGetMemoryBlockAddr", {0xffffffffu, Fixture::output}) == mhp3rd::error::kIllegalMemblock,
        "block lookup handles optional pointer and bad UID");
    check(call("sceKernelFreePartitionMemory", {uid}) == 0, "partition free succeeds");
    check(call("sceKernelAllocMemoryBlock", {Fixture::text, 2, 256}) == mhp3rd::error::kIllegalMemblockType,
        "new allocation API rejects fixed type");
    const auto newer = call("sceKernelAllocMemoryBlock", {Fixture::text, 1, 256});
    check(static_cast<int>(newer) > 0 && call("sceKernelFreeMemoryBlock", {newer}) == 0,
        "new allocation API allocates and frees");
    for (const auto *name : {"sceKernelSetCompilerVersion", "sceKernelSetCompiledSdkVersion603_605"})
        check(call(name) == 0, "compiler metadata acknowledged");
    check(f.call("sceSuspendForUser", "sceKernelVolatileMemLock", {0, Fixture::output, Fixture::output + 4}) == 0,
        "volatile lock succeeds");
    check(f.runtime.memory().load32(Fixture::output) == mhp3rd::kVolatileMemoryBase &&
            f.runtime.memory().load32(Fixture::output + 4) == mhp3rd::kVolatileMemorySize,
        "volatile lock returns reserved region");
    check(f.call("sceSuspendForUser", "sceKernelVolatileMemLock") == 0 &&
            f.call("sceSuspendForUser", "sceKernelVolatileMemUnlock") == 0 &&
            f.call("sceSuspendForUser", "sceKernelPowerTick") == 0,
        "optional lock pointers and power operations succeed");
    f.string(Fixture::output, "abcdefghij");
    check(f.call("sceDmac", "sceDmacMemcpy", {Fixture::output + 2, Fixture::output, 8}) == 0 &&
            mhp3rd::read_cstring(f.runtime.memory(), Fixture::output, 10) == "ababcdefgh",
        "backward overlapping copy preserves bytes");
    check(f.call("sceDmac", "sceDmacMemcpy", {Fixture::output, Fixture::output + 2, 8}) == 0 &&
            mhp3rd::read_cstring(f.runtime.memory(), Fixture::output, 8) == "abcdefgh",
        "forward overlapping copy preserves bytes");
    check(f.call("sceDmac", "sceDmacMemcpy", {Fixture::output, Fixture::output, 0}) == 0, "empty DMA copy succeeds");
    f.string(Fixture::text, "%d %i %u %x %X %p %c %% %q %.");
    check(call("sceKernelPrintf", {Fixture::text, 0xffffffffu, 2, 3, 4, 5, 6, 'z'}) == 0,
        "guest printf safely consumes bounded register arguments");
    f.string(Fixture::text, "%s");
    check(call("sceKernelPrintf", {Fixture::text, Fixture::output}) == 0, "guest printf reads string argument");
}
class PublicFiles {
public:
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        ("yakumo-kernel-contract-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    PublicFiles() { std::filesystem::create_directories(root / "ms"); }
    ~PublicFiles() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path iso() {
        std::vector<std::uint8_t> bytes(24u * 2048u);
        auto le32 = [&](std::size_t offset, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
        };
        bytes[16u * 2048u] = 1;
        const std::string signature = "CD001";
        std::copy(signature.begin(), signature.end(), bytes.begin() + 16u * 2048u + 1);
        le32(16u * 2048u + 158, 20);
        le32(16u * 2048u + 166, 2048);
        const std::string filename = "PUBLIC.TXT;1";
        const auto record = 20u * 2048u;
        bytes[record] = static_cast<std::uint8_t>(33 + filename.size());
        le32(record + 2, 21);
        le32(record + 10, 8);
        bytes[record + 32] = static_cast<std::uint8_t>(filename.size());
        std::copy(filename.begin(), filename.end(), bytes.begin() + record + 33);
        const std::string payload = "fixture!";
        std::copy(payload.begin(), payload.end(), bytes.begin() + 21u * 2048u);
        const auto path = root / "public.iso";
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return path;
    }
};
void io_contracts() {
    Fixture f;
    PublicFiles files;
    mhp3rd::HleRegistrar hle(f.runtime);
    mhp3rd::register_io(hle, {}, files.root / "ms");
    auto call = [&](const char *name, std::initializer_list<unsigned> args = {}) {
        return f.call("IoFileMgrForUser", name, args);
    };
    auto path_call = [&](const char *name, std::string_view path, unsigned second = 0) {
        f.string(Fixture::text, path);
        return call(name, {Fixture::text, second});
    };
    constexpr unsigned badfd = 0x80010009u, missing = 0x80010002u, device = 0x80010013u, invalid = 0x80010016u,
                       readonly = 0x8001001eu;
    check(path_call("sceIoOpen", "unknown:/missing", 1) == device &&
            path_call("sceIoOpen", "disc0:/public.txt", 1) == device,
        "unsupported device and absent disc rejected");
    check(f.call("sceUmdUser", "sceUmdGetDriveStat") == 1, "absent disc reports no media");
    check(path_call("sceIoOpen", "ms0:/missing", 1) == missing, "missing memory-stick file rejected");
    const auto fd = path_call("sceIoOpen", "MS0:\\folder\\..\\folder\\sample.txt", 0x602);
    check(static_cast<int>(fd) > 0, "create normalizes path and creates parent directories");
    f.string(Fixture::output, "synthetic data");
    check(call("sceIoWrite", {fd, Fixture::output, 14}) == 14, "host write returns exact length");
    check(call("sceIoLseek", {fd, 0, 0, 0, 0}) == 0 && f.ctx.gpr[3] == 0, "seek to start returns 64-bit offset");
    check(call("sceIoRead", {fd, Fixture::output, 50}) == 14 &&
            mhp3rd::read_cstring(f.runtime.memory(), Fixture::output) == "synthetic data",
        "read clamps at EOF and preserves bytes");
    check(call("sceIoRead", {fd, Fixture::output, 1}) == 0, "EOF read returns zero");
    std::array<std::uint8_t, 5> prefix{};
    check(mhp3rd::read_open_file(fd, 0, prefix.data(), prefix.size()) == 5 &&
            std::string(prefix.begin(), prefix.end()) == "synth",
        "position-independent read returns requested prefix");
    check(call("sceIoLseek", {fd, 0, 0, 0, 1}) == 14, "position-independent read does not move guest offset");
    check(call("sceIoLseek", {fd, 0, 0xfffffffbu, 0xffffffffu, 2}) == 9, "negative offset relative to EOF succeeds");
    check(call("sceIoLseek", {fd, 0, 1, 0, 1}) == 10, "seek relative to current offset succeeds");
    check(call("sceIoLseek", {fd, 0, 0xffffffffu, 0xffffffffu, 0}) == invalid && f.ctx.gpr[3] == 0xffffffffu,
        "negative absolute seek returns signed wide error");
    check(call("sceIoLseek", {fd, 0, 0, 0, 9}) == invalid, "invalid seek base rejected");
    check(path_call("sceIoGetstat", "fatms0:/folder/sample.txt", Fixture::output) == 0,
        "stat accepts memory-stick alias");
    check(f.runtime.memory().load32(Fixture::output) == 0x21ff &&
            f.runtime.memory().load32(Fixture::output + 8) == 14 &&
            f.runtime.memory().load32(Fixture::output + 12) == 0,
        "stat reports file type and 64-bit size");
    check(path_call("sceIoGetstat", "ms0:/folder", Fixture::output) == 0 &&
            f.runtime.memory().load32(Fixture::output) == 0x11ff,
        "directory stat reports directory mode");
    const auto directory = path_call("sceIoDopen", "ms0:/folder");
    check(static_cast<int>(directory) > 0 && call("sceIoRead", {directory, Fixture::output, 1}) == badfd &&
            call("sceIoWrite", {directory, Fixture::output, 1}) == badfd,
        "directory descriptors reject byte I/O");
    check(mhp3rd::read_open_file(directory, 0, prefix.data(), 5) == 0, "position-independent read rejects directories");
    check(call("sceIoDclose", {directory}) == 0 && call("sceIoDclose", {directory}) == badfd,
        "directory close validates descriptor");
    check(path_call("sceIoDopen", "ms0:/missing") == missing &&
            path_call("sceIoGetstat", "ms0:/missing", Fixture::output) == missing,
        "missing directory/stat errors");
    check(call("sceIoClose", {fd}) == 0 && call("sceIoClose", {fd}) == badfd, "double close rejected");
    check(mhp3rd::read_open_file(fd, 0, prefix.data(), 5) == 0, "closed file cannot be read by module loader");
    for (const auto *name : {"sceIoRead", "sceIoWrite", "sceIoLseek"})
        check(call(name, {fd, Fixture::output, 1}) == badfd, "closed descriptor operations rejected");
    f.string(Fixture::text, "ms0:/folder/sample.txt");
    f.string(Fixture::output, "ms0:/folder/renamed.txt");
    check(call("sceIoRename", {Fixture::text, Fixture::output}) == 0 &&
            std::filesystem::exists(files.root / "ms/folder/renamed.txt"),
        "rename changes host file");
    check(call("sceIoRename", {Fixture::text, Fixture::output}) == missing, "rename missing source fails");
    f.string(Fixture::text, "disc0:/public.txt");
    check(call("sceIoRename", {Fixture::text, Fixture::output}) == readonly, "cross-device rename rejected");
    mhp3rd::register_io(hle, files.iso(), files.root / "ms");
    check(f.call("sceUmdUser", "sceUmdGetDriveStat") == 0x32 && f.call("sceUmdUser", "sceUmdActivate") == 0 &&
            f.call("sceUmdUser", "sceUmdGetErrorStat") == 0,
        "synthetic disc reports ready/readable");
    check(path_call("sceIoOpen", "disc0:/public.txt", 2) == readonly &&
            path_call("sceIoOpen", "disc0:/missing", 1) == missing,
        "disc rejects writes and missing files");
    const auto disc = path_call("sceIoOpen", "umd0:/public.txt", 1);
    check(static_cast<int>(disc) > 0 && call("sceIoRead", {disc, Fixture::output, 50}) == 8 &&
            mhp3rd::read_cstring(f.runtime.memory(), Fixture::output, 8) == "fixture!",
        "disc reads synthetic payload and clamps requested length");
    check(call("sceIoRead", {disc, Fixture::output, 1}) == 0 && mhp3rd::read_open_file(disc, 8, prefix.data(), 5) == 0,
        "disc EOF applies to both read interfaces");
    check(
        mhp3rd::read_open_file(disc, 2, prefix.data(), 5) == 5 && std::string(prefix.begin(), prefix.end()) == "xture",
        "disc independent read addresses original extent");
    check(path_call("sceIoGetstat", "isofs0:/PUBLIC.TXT", Fixture::output) == 0 &&
            f.runtime.memory().load32(Fixture::output + 8) == 8 &&
            f.runtime.memory().load32(Fixture::output + 64) == 21,
        "disc stat returns extent and length");
    const auto raw = path_call("sceIoOpen", "disc0:/sce_lbn0x15_size0x8", 1);
    check(static_cast<int>(raw) > 0 && call("sceIoRead", {raw, Fixture::output, 8}) == 8,
        "raw hexadecimal sector path reads synthetic payload");
    const auto rawdecimal = path_call("sceIoOpen", "disc0:/sce_lbn21_size8", 1);
    check(static_cast<int>(rawdecimal) > 0 && call("sceIoRead", {rawdecimal, Fixture::output, 8}) == 8,
        "decimal sector path accepts complete numbers");
    check(path_call("sceIoOpen", "disc0:/sce_lbnxx_size8", 1) == missing &&
            path_call("sceIoOpen", "disc0:/sce_lbn21_size8x", 1) == missing,
        "malformed sector numbers fail cleanly");
    const auto rawdevice = path_call("sceIoOpen", "disc0:", 1);
    check(static_cast<int>(rawdevice) > 0 && call("sceIoLseek", {rawdevice, 0, 0, 0, 2}) == 24 * 2048,
        "raw device exposes whole image length");
    const auto root = path_call("sceIoDopen", "disc0:");
    check(static_cast<int>(root) > 0, "disc root opens as directory");
    for (auto open : {disc, raw, rawdecimal, rawdevice, root})
        check(call("sceIoClose", {open}) == 0, "all synthetic descriptors close");
    f.string(Fixture::text, "ms0:");
    for (const auto command : {0x02025801u, 0x02025806u, 0x02425823u}) {
        f.runtime.memory().store32(Fixture::output, 99);
        check(call("sceIoDevctl", {Fixture::text, command, 0, 0, Fixture::output, 4}) == 0 &&
                f.runtime.memory().load32(Fixture::output) == (command == 0x02025801u ? 4u : 1u),
            "memory-stick devctl reports inserted state");
        f.runtime.memory().store32(Fixture::output, 99);
        call("sceIoDevctl", {Fixture::text, command, 0, 0, Fixture::output, 3});
        check(f.runtime.memory().load32(Fixture::output) == 99, "short devctl output remains untouched");
    }
    f.runtime.memory().store32(Fixture::output, Fixture::output + 16);
    check(call("sceIoDevctl", {Fixture::text, 0x02425818u, Fixture::output}) == 0 &&
            f.runtime.memory().load32(Fixture::output + 28) == 512 &&
            f.runtime.memory().load32(Fixture::output + 32) == 32,
        "free-space geometry written through nested pointer");
    const auto cb = f.thread("sceKernelCreateCallback", {0, 0x08826000, 0});
    f.runtime.memory().store32(Fixture::output, cb);
    check(call("sceIoDevctl", {Fixture::text, 0x02015804u, Fixture::output}) == 0 &&
            f.kernel.callbacks.at(cb).notify_argument == 1,
        "memory-stick insertion callback notified immediately");
    for (const auto command : {0x02415821u, 0x02015805u, 0x02415822u, 0x02425818u, 123u})
        check(call("sceIoDevctl", {Fixture::text, command}) == 0,
            "optional devctl pointers and unsupported command acknowledged");
    f.string(Fixture::output, "log");
    check(call("sceIoWrite", {1, Fixture::output, 3}) == 3 && call("sceIoWrite", {2, Fixture::output, 0}) == 0,
        "stdio handles guest output and empty writes");
}

}
int main() {
    try {
        memory_contracts();
        scheduler_contracts();
        semaphore_contracts();
        event_flag_contracts();
        mutex_contracts();
        interrupt_and_host_wait_contracts();
        vtimer_contracts();
        sysmem_contracts();
        io_contracts();
        std::cout << "kernel/HLE contracts passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
