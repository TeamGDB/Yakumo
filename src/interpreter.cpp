#include "psprecomp/interpreter.hpp"

#include "allegrex_semantics.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/decoder.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace psprecomp {

namespace {

bool g_settings_loaded = false;
bool g_enabled = true;
bool g_verbose = false;
std::uint64_t g_budget = 1024u;
std::uint64_t g_instructions = 0u;
std::uint64_t g_entries = 0u;
std::size_t g_announced = 0u;
// Bounded so a title that interprets thousands of distinct overlay addresses
// cannot turn the diagnostic into a memory leak.
constexpr std::size_t kMaxProfiledEntries = 4096u;
std::unordered_map<std::uint32_t, std::uint64_t> g_entry_profile;

void load_settings() {
    if (g_settings_loaded) return;
    g_settings_loaded = true;
    g_enabled = std::getenv("PSPRECOMP_NO_INTERPRETER") == nullptr;
    g_verbose = std::getenv("PSPRECOMP_INTERPRETER_VERBOSE") != nullptr;
    if (const char *text = std::getenv("PSPRECOMP_INTERPRETER_BUDGET")) {
        char *end = nullptr;
        const unsigned long long value = std::strtoull(text, &end, 0);
        if (end != text && *end == '\0' && value != 0ull) g_budget = static_cast<std::uint64_t>(value);
    }
}

void note_entry(std::uint32_t pc) {
    ++g_entries;
    if (g_entry_profile.contains(pc) || g_entry_profile.size() >= kMaxProfiledEntries) return;
    g_entry_profile.emplace(pc, 0u);
    // Announce the first fallback unconditionally: an unexpected one is the
    // difference between "the corpus is incomplete" and "the game is slow".
    if (g_announced == 0u || g_verbose) {
        std::cerr << "[interpreter] no recompiled function at " << hex32(pc) << "; interpreting guest code instead\n";
    }
    ++g_announced;
}

void charge_entry(std::uint32_t pc, std::uint64_t instructions) {
    const auto found = g_entry_profile.find(pc);
    if (found != g_entry_profile.end()) found->second += instructions;
}

} // namespace

bool interpreter_fallback_enabled() noexcept {
    load_settings();
    return g_enabled;
}

void set_interpreter_fallback_enabled(bool enabled) noexcept {
    load_settings();
    g_enabled = enabled;
}

std::uint64_t interpreter_instruction_budget() noexcept {
    load_settings();
    return g_budget;
}

InterpreterStats interpreter_stats() noexcept {
    return InterpreterStats{g_instructions, g_entries, g_entry_profile.size()};
}

std::vector<std::pair<std::uint32_t, std::uint64_t>> interpreter_entry_profile() {
    std::vector<std::pair<std::uint32_t, std::uint64_t>> entries(g_entry_profile.begin(), g_entry_profile.end());
    std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
        if (left.second != right.second) return left.second > right.second;
        return left.first < right.first;
    });
    return entries;
}

void reset_interpreter_stats() noexcept {
    g_instructions = 0u;
    g_entries = 0u;
    g_announced = 0u;
    g_entry_profile.clear();
}

void report_interpreter_stats(std::size_t limit) {
    if (g_instructions == 0u) return;
    std::cerr << "[interpreter] instructions=" << g_instructions << " entries=" << g_entries
              << " unique_addresses=" << g_entry_profile.size() << "\n";
    const auto entries = interpreter_entry_profile();
    for (std::size_t index = 0u; index < std::min(limit, entries.size()); ++index) {
        std::cerr << "[interpreter] " << hex32(entries[index].first) << " instructions=" << entries[index].second
                  << "\n";
    }
}

InterpreterExit interpret_allegrex(Runtime &rt, AllegrexContext &ctx, std::uint64_t instruction_budget) {
    load_settings();
    const std::uint64_t budget = instruction_budget != 0u ? instruction_budget : g_budget;
    GuestMemory &memory = rt.memory();

    std::uint32_t pc = ctx.pc;
    if (!memory.contains(pc, 4u)) return InterpreterExit::Unreachable;

    const std::uint32_t entry_pc = pc;
    note_entry(entry_pc);

    std::uint64_t executed = 0u;
    const auto leave = [&](InterpreterExit exit) {
        g_instructions += executed;
        charge_entry(entry_pc, executed);
        return exit;
    };

    while (true) {
        // A registered address may be an ordinary AOT unit, a profile override
        // or a PSP import stub. All three must run through normal dispatch, so
        // interpreted code reaches invoke_import_cached exactly the way
        // generated code does: by leaving with ctx.pc at the stub.
        if (executed != 0u && rt.has_function(pc)) {
            ctx.pc = pc;
            return leave(InterpreterExit::Dispatch);
        }
        if (executed >= budget) {
            ctx.pc = pc;
            return leave(InterpreterExit::Budget);
        }
        if (!memory.contains(pc, 4u)) {
            ctx.pc = pc;
            rt.stop("Interpreter reached unmapped guest address " + hex32(pc));
            return leave(InterpreterExit::Stopped);
        }

        ctx.pc = pc;
        const DecodedInstruction decoded = decode_allegrex(memory.aot_load32(pc));
        ++executed;

        if (!decoded.has_delay_slot()) {
            if (!semantics::execute_simple(rt, ctx, decoded, pc))
                return leave(rt.stopped() ? InterpreterExit::Stopped : InterpreterExit::Dispatch);
            pc += 4u;
            continue;
        }

        if (!memory.contains(pc + 4u, 4u)) {
            ctx.pc = pc + 4u;
            rt.stop("Interpreter reached unmapped delay slot at " + hex32(pc + 4u));
            return leave(InterpreterExit::Stopped);
        }
        const DecodedInstruction slot = decode_allegrex(memory.aot_load32(pc + 4u));
        if (slot.is_control_flow()) {
            ctx.pc = pc + 4u;
            rt.unsupported(pc + 4u, slot.word, "control flow in delay slot");
            return leave(rt.stopped() ? InterpreterExit::Stopped : InterpreterExit::Dispatch);
        }

        const std::uint32_t return_pc = pc + 8u;
        std::uint32_t next = return_pc;
        bool run_slot = true;
        switch (decoded.kind) {
        case OpcodeKind::J:
        case OpcodeKind::Jal:
            if (decoded.kind == OpcodeKind::Jal) ctx.set_gpr(31u, return_pc);
            next = ((pc + 4u) & 0xF0000000u) | (decoded.target << 2u);
            break;
        case OpcodeKind::Jr:
        case OpcodeKind::Jalr: {
            // Both operands are read before the delay slot runs, so a slot that
            // overwrites rs or the link register cannot change the transfer.
            const std::uint32_t jump_target = ctx.gpr[decoded.rs];
            if (decoded.kind == OpcodeKind::Jalr) ctx.set_gpr(decoded.rd == 0u ? 31u : decoded.rd, return_pc);
            next = jump_target;
            break;
        }
        default: {
            // The condition is evaluated before the link write, because a
            // branch-and-link whose condition register is $ra must test the old
            // value. A likely branch annuls its delay slot when not taken.
            const bool taken = semantics::evaluate_branch(ctx, decoded);
            if (semantics::is_link_branch(decoded.kind)) ctx.set_gpr(31u, return_pc);
            if (taken) {
                next = pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(decoded.immediate) * 4);
            }
            run_slot = taken || !semantics::is_likely_branch(decoded.kind);
            break;
        }
        }

        if (run_slot) {
            ++executed;
            ctx.pc = pc + 4u;
            if (!semantics::execute_simple(rt, ctx, slot, pc + 4u))
                return leave(rt.stopped() ? InterpreterExit::Stopped : InterpreterExit::Dispatch);
        }
        pc = next;
    }
}

} // namespace psprecomp
