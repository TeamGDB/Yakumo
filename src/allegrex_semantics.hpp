#pragma once

#include "psprecomp/decoder.hpp"

#include <cstdint>

namespace psprecomp {
struct AllegrexContext;
class Runtime;
} // namespace psprecomp

// The meaning of each Allegrex instruction, shared by the interpreter and the
// JIT so that both run guest code from memory exactly alike. Internal to the
// runtime library: generated code and overlays never include this header.
namespace psprecomp::semantics {

// Branch-likely forms, which annul their delay slot when not taken.
[[nodiscard]] bool is_likely_branch(OpcodeKind kind) noexcept;
// Branch-and-link forms, which write $ra whether taken or not.
[[nodiscard]] bool is_link_branch(OpcodeKind kind) noexcept;
// Whether a conditional branch is taken, from the registers before its delay
// slot runs.
[[nodiscard]] bool evaluate_branch(const AllegrexContext &ctx, const DecodedInstruction &d) noexcept;

// Executes one instruction that does not transfer control. Returns false when
// the runtime stopped or an unsupported-instruction hook asked for a retry;
// ctx.pc is then the instruction's address and the caller returns to the
// outer dispatcher.
bool execute_simple(Runtime &rt, AllegrexContext &ctx, const DecodedInstruction &d, std::uint32_t pc);

} // namespace psprecomp::semantics
