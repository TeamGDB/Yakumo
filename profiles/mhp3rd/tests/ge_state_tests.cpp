#include "gpu/ge_state.hpp"
#include "perf/frame_stats.hpp"
#include "psprecomp/common.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace mhp3rd::perf {
bool alternate_off(NewPath) { return false; }
bool split_enabled() noexcept { return false; }
std::uint64_t split_ticks() noexcept { return 0u; }
void add_split(Split, std::uint64_t) noexcept {}
}

namespace {

int failures{};

void expect(bool condition, const char *message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void test_nested_call_restores_offset() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08801000u;
    constexpr std::uint32_t first = 0x08801100u;
    constexpr std::uint32_t nested = 0x08811100u;
    constexpr std::uint32_t second = 0x08801200u;
    constexpr std::uint32_t wrong_second = 0x08821200u;

    memory.store32(main, 0x10080000u);       // BASE 0x08000000
    memory.store32(main + 4u, 0x0A801100u);  // CALL first
    memory.store32(main + 8u, 0x0A801200u);  // CALL second after both RETs
    memory.store32(main + 12u, 0x0F000001u); // FINISH callback
    memory.store32(main + 16u, 0x0C000000u); // END

    memory.store32(first, 0x13000100u);       // OFFSET_ADDR 0x10000
    memory.store32(first + 4u, 0x10080000u);  // BASE remains 0x08000000
    memory.store32(first + 8u, 0x0A801100u);  // CALL nested at 0x08811100
    memory.store32(first + 12u, 0x0B000000u); // RET
    memory.store32(nested, 0x13000200u);      // OFFSET_ADDR 0x20000
    memory.store32(nested + 4u, 0x0B000000u); // RET
    memory.store32(second, 0x0B000000u);      // RET to FINISH
    memory.store32(wrong_second, 0x0C000000u); // Wrong target terminates without FINISH

    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState first_state;
    unsigned finishes = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) {
        if ((signal & 0x10000u) != 0u) ++finishes;
    });
    bool done = false;
    const std::uint32_t next = ge.execute(memory, first_state, main, 0u, done);
    expect(done && next == main + 20u, "nested CALL/RET reaches outer END");
    expect(finishes == 1u, "nested CALL/RET restores OFFSET_ADDR before outer FINISH");

    memory.store32(main + 0x1000u, 0x10080000u);
    memory.store32(main + 0x1004u, 0x0A801200u);
    memory.store32(main + 0x1008u, 0x0F000002u);
    memory.store32(main + 0x100Cu, 0x0C000000u);
    done = false;
    mhp3rd::gpu::GeState::ListExecutionState second_state;
    ge.execute(memory, second_state, main + 0x1000u, 0u, done);
    expect(done && finishes == 2u, "later display list reaches its own FINISH");
}

void test_bare_end_inside_call_does_not_finish_list() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08802000u;
    constexpr std::uint32_t sublist = 0x08802100u;
    memory.store32(main, 0x10080000u);          // BASE
    memory.store32(main + 4u, 0x0A802100u);     // CALL sublist
    memory.store32(main + 8u, 0x0F000001u);     // FINISH after RET
    memory.store32(main + 12u, 0x0C000000u);    // END after FINISH
    memory.store32(sublist, 0xF1D00018u);       // Unhandled command before bare END
    memory.store32(sublist + 4u, 0x0C8C5E12u);  // Bare END with nonzero payload
    memory.store32(sublist + 8u, 0x0B000000u);  // RET

    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    unsigned finishes = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) {
        if ((signal & 0x10000u) != 0u) ++finishes;
    });
    bool done = false;
    const std::uint32_t next = ge.execute(memory, list_state, main, 0u, done);
    expect(done && next == main + 16u, "bare END inside CALL continues through RET to outer END");
    expect(finishes == 1u, "bare END inside CALL preserves exactly one outer FINISH callback");
}

void test_finish_end_across_stall() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08803000u;
    memory.store32(main, 0x0F000001u);
    memory.store32(main + 4u, 0x0C000000u);
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    unsigned finishes = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) {
        if ((signal & 0x10000u) != 0u) ++finishes;
    });
    bool done = false;
    const std::uint32_t stalled = ge.execute(memory, list_state, main, main + 4u, done);
    expect(!done && stalled == main + 4u && finishes == 1u, "list stalls after FINISH before END");
    const std::uint32_t next = ge.execute(memory, list_state, stalled, 0u, done);
    expect(done && next == main + 8u && finishes == 1u, "END after resumed FINISH completes without another callback");
}

void test_top_level_bare_end_does_not_finish_early() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08803500u;
    memory.store32(main, 0x0CFFFFFFu);          // Bare END must continue.
    memory.store32(main + 4u, 0x0E000123u);    // Actual command after END.
    memory.store32(main + 8u, 0x0F000004u);    // FINISH.
    memory.store32(main + 12u, 0x0C000000u);   // Real FINISH/END completion.
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    std::vector<std::uint32_t> signals;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) { signals.push_back(signal); });
    bool done = false;
    const std::uint32_t next = ge.execute(memory, list_state, main, 0u, done);
    expect(done && next == main + 16u, "top-level bare END continues through commands to FINISH/END");
    expect(signals.size() == 2u && signals[0] == 0x123u && signals[1] == 0x10004u,
           "commands after bare END and the real FINISH callback both execute");
}

void test_interleaved_list_completion_preserves_stalled_call() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t list_a = 0x08807000u;
    constexpr std::uint32_t child_a = 0x08807100u;
    constexpr std::uint32_t continuation_a = 0x08807200u;
    constexpr std::uint32_t list_b = 0x08807300u;

    memory.store32(list_a, 0x10080000u);               // BASE
    memory.store32(list_a + 4u, 0x0A807100u);          // CALL child A
    memory.store32(list_a + 8u, 0x0A807200u);          // CALL continuation after child RET
    memory.store32(list_a + 12u, 0x0F000001u);         // FINISH
    memory.store32(list_a + 16u, 0x0C000000u);         // END
    memory.store32(child_a, 0x13000100u);              // OFFSET_ADDR 0x10000
    memory.store32(child_a + 4u, 0x0B000000u);         // RET; list A stalls here
    memory.store32(child_a + 8u, 0x0F000011u);         // Wrong path if A's frame was lost
    memory.store32(child_a + 12u, 0x0C000000u);
    memory.store32(continuation_a, 0x0B000000u);       // RET
    memory.store32(list_b, 0x13000200u);               // OFFSET_ADDR 0x20000
    memory.store32(list_b + 4u, 0x0F000002u);          // FINISH
    memory.store32(list_b + 8u, 0x0C000000u);          // END

    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState state_a;
    mhp3rd::gpu::GeState::ListExecutionState state_b;
    unsigned finishes = 0u;
    std::uint32_t last_finish_pc = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t pc) {
        if ((signal & 0x10000u) != 0u) {
            ++finishes;
            last_finish_pc = pc;
        }
    });
    bool done_a = false;
    const std::uint32_t stalled = ge.execute(memory, state_a, list_a, child_a + 4u, done_a);
    expect(!done_a && stalled == child_a + 4u, "list A stalls with its CALL frame at RET");

    bool done_b = false;
    const std::uint32_t next_b = ge.execute(memory, state_b, list_b, 0u, done_b);
    expect(done_b && next_b == list_b + 12u, "list B completes while list A remains stalled");

    done_a = false;
    const std::uint32_t next_a = ge.execute(memory, state_a, stalled, 0u, done_a);
    expect(done_a && next_a == list_a + 20u, "resumed list A returns through its saved frame to its own END");
    expect(finishes == 2u && last_finish_pc == list_a + 16u,
           "list A restores OFFSET_ADDR and reaches its own post-RET continuation");
}

void test_invalid_list_reports_address() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08804000u;
    memory.store32(main, 0x10050000u);      // BASE 0x05000000 is unmapped.
    memory.store32(main + 4u, 0x0A000000u); // CALL that address.
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    bool done = false;
    try {
        (void)ge.execute(memory, list_state, main, 0u, done);
        expect(false, "invalid GE CALL must fail loudly");
    } catch (const psprecomp::Error &error) {
        const std::string message = error.what();
        expect(message.find("0x08804000") != std::string::npos &&
               message.find("0x05000000") != std::string::npos && !done,
               "invalid GE CALL identifies list start and unmapped target");
    }
}

void test_completed_call_does_not_leak_into_next_list() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t first = 0x08805000u;
    constexpr std::uint32_t sublist = 0x08805100u;
    constexpr std::uint32_t second = 0x08805200u;
    memory.store32(first, 0x10080000u);
    memory.store32(first + 4u, 0x0A805100u);
    memory.store32(first + 8u, 0x0C000000u);
    memory.store32(sublist, 0x0F000001u);
    memory.store32(sublist + 4u, 0x0C000000u); // FINISH+END at CALL depth one.
    memory.store32(second, 0x0B000000u);      // Empty RET must not return to first list.
    memory.store32(second + 4u, 0x0F000002u);
    memory.store32(second + 8u, 0x0C000000u);
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState first_state;
    mhp3rd::gpu::GeState::ListExecutionState second_state;
    unsigned finishes = 0u;
    ge.set_signal_sink([&](std::uint32_t signal, std::uint32_t) {
        if ((signal & 0x10000u) != 0u) ++finishes;
    });
    bool done = false;
    (void)ge.execute(memory, first_state, first, 0u, done);
    expect(done && finishes == 1u, "called sublist may finish its list");
    done = false;
    const auto next = ge.execute(memory, second_state, second, 0u, done);
    expect(done && next == second + 12u && finishes == 2u,
           "completed CALL frames do not redirect a later list");
}

void test_command_budget_fails_loudly() {
    psprecomp::GuestMemory memory;
    constexpr std::uint32_t main = 0x08806000u;
    memory.store32(main, 0x10080000u);      // BASE 0x08000000.
    memory.store32(main + 4u, 0x08806004u); // JUMP to itself.
    mhp3rd::gpu::GeState ge;
    mhp3rd::gpu::GeState::ListExecutionState list_state;
    bool done = false;
    try {
        (void)ge.execute(memory, list_state, main, 0u, done);
        expect(false, "cyclic GE list must fail at the command budget");
    } catch (const psprecomp::Error &error) {
        const std::string message = error.what();
        expect(message.find("exceeded 2000000 commands") != std::string::npos &&
               message.find("0x08806000") != std::string::npos && !done,
               "command-budget error reports the list and last command location");
    }
}

} // namespace

int main() {
    test_nested_call_restores_offset();
    test_bare_end_inside_call_does_not_finish_list();
    test_finish_end_across_stall();
    test_top_level_bare_end_does_not_finish_early();
    test_interleaved_list_completion_preserves_stalled_call();
    test_invalid_list_reports_address();
    test_completed_call_does_not_leak_into_next_list();
    test_command_budget_fails_loudly();
    if (failures != 0) std::cerr << failures << " GE state test(s) failed\n";
    return failures == 0 ? 0 : 1;
}
