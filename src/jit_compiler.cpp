// Compiles one guest function from memory to native code (see
// jit_compiler.hpp and psprecomp/jit.hpp).
//
// One compiled function covers the code reachable from its entry inside the
// entry's 16 KiB window. The guest registers it uses are loaded from the
// AllegrexContext into host registers on entry, kept there across the
// function, and stored back wherever it leaves and before host code that reads
// the context runs; branches between instructions of the function are native
// jumps, everything else leaves through ctx.pc as a generated unit does. Loads
// and stores take the RAM fast path inline and call the GuestMemory accessors
// otherwise. Single-precision arithmetic, moves and compares run as native
// floating-point instructions; instructions the emitter does not know run
// through semantics::execute_simple(), the interpreter's own definition.
#include "jit_compiler.hpp"

#include "allegrex_semantics.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <asmjit/ujit.h>

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <optional>

namespace psprecomp::jit {
namespace {

using namespace asmjit;
using ujit::Gp;
using ujit::Mem;
using ujit::Vec;
using ujit::UniCompiler;
using ujit::UniCondition;

constexpr std::size_t kMaxInstructions = 4096u;

// Set when a helper met an error and stopped the runtime: compiled code checks
// it after the calls that can fail, and returns.
std::uint8_t g_fault = 0u;

// Jumps back left before compiled code returns to the dispatcher; shared by
// all compiled code, which runs on one thread.
std::uint32_t g_back_edges = kBackEdges;

JitInstructionHook g_instruction_hook = nullptr;

// --- Helpers called from compiled code --------------------------------------
// Plain functions with integer and pointer arguments only. An exception must
// never unwind through compiled code: each helper catches, stops the runtime
// and raises g_fault.

void fail(Runtime *runtime, const char *what) {
    g_fault = 1u;
    runtime->stop(std::string("guest code compiled at run time stopped: ") + what);
}

// The guest instruction a memory error happened at, for the message.
std::string at_pc(const char *what, std::uint32_t pc) {
    return std::string(what) + " (at guest pc " + hex32(pc) + ")";
}

std::uint32_t load8_slow(Runtime *runtime, std::uint32_t address, std::uint32_t pc) {
    try {
        return runtime->memory().aot_load8(address);
    } catch (const std::exception &error) {
        fail(runtime, at_pc(error.what(), pc).c_str());
        return 0u;
    } catch (...) {
        fail(runtime, at_pc("an unknown error", pc).c_str());
        return 0u;
    }
}
std::uint32_t load8s_slow(Runtime *runtime, std::uint32_t address, std::uint32_t pc) {
    return static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int8_t>(load8_slow(runtime, address, pc))));
}
std::uint32_t load16_slow(Runtime *runtime, std::uint32_t address, std::uint32_t pc) {
    try {
        return runtime->memory().aot_load16(address);
    } catch (const std::exception &error) {
        fail(runtime, at_pc(error.what(), pc).c_str());
        return 0u;
    } catch (...) {
        fail(runtime, at_pc("an unknown error", pc).c_str());
        return 0u;
    }
}
std::uint32_t load16s_slow(Runtime *runtime, std::uint32_t address, std::uint32_t pc) {
    return static_cast<std::uint32_t>(
        static_cast<std::int32_t>(static_cast<std::int16_t>(load16_slow(runtime, address, pc))));
}
std::uint32_t load32_slow(Runtime *runtime, std::uint32_t address, std::uint32_t pc) {
    try {
        return runtime->memory().aot_load32(address);
    } catch (const std::exception &error) {
        fail(runtime, at_pc(error.what(), pc).c_str());
        return 0u;
    } catch (...) {
        fail(runtime, at_pc("an unknown error", pc).c_str());
        return 0u;
    }
}
void store8_slow(Runtime *runtime, std::uint32_t address, std::uint32_t value, std::uint32_t pc) {
    try {
        runtime->memory().aot_store8(address, static_cast<std::uint8_t>(value));
    } catch (const std::exception &error) {
        fail(runtime, at_pc(error.what(), pc).c_str());
    } catch (...) {
        fail(runtime, at_pc("an unknown error", pc).c_str());
    }
}
void store16_slow(Runtime *runtime, std::uint32_t address, std::uint32_t value, std::uint32_t pc) {
    try {
        runtime->memory().aot_store16(address, static_cast<std::uint16_t>(value));
    } catch (const std::exception &error) {
        fail(runtime, at_pc(error.what(), pc).c_str());
    } catch (...) {
        fail(runtime, at_pc("an unknown error", pc).c_str());
    }
}
void store32_slow(Runtime *runtime, std::uint32_t address, std::uint32_t value, std::uint32_t pc) {
    try {
        runtime->memory().aot_store32(address, value);
    } catch (const std::exception &error) {
        fail(runtime, at_pc(error.what(), pc).c_str());
    } catch (...) {
        fail(runtime, at_pc("an unknown error", pc).c_str());
    }
}

// 1 when the instruction ran; 0 when the caller must return to the dispatcher
// with ctx.pc as left (the runtime stopped, or a hook asked for a retry).
std::uint32_t run_shared(
    Runtime *runtime, AllegrexContext *ctx, const DecodedInstruction *instruction, std::uint32_t pc) {
    try {
        return semantics::execute_simple(*runtime, *ctx, *instruction, pc) ? 1u : 0u;
    } catch (const std::exception &error) {
        ctx->pc = pc;
        fail(runtime, error.what());
        return 0u;
    } catch (...) {
        ctx->pc = pc;
        fail(runtime, "an unknown error");
        return 0u;
    }
}

// --- Finding the code --------------------------------------------------------

struct Node {
    DecodedInstruction instruction;
    DecodedInstruction slot; // for instructions with a delay slot
};

bool is_conditional(OpcodeKind kind) {
    switch (kind) {
    case OpcodeKind::J:
    case OpcodeKind::Jal:
    case OpcodeKind::Jr:
    case OpcodeKind::Jalr:
        return false;
    default:
        return true;
    }
}

std::uint32_t branch_target(const DecodedInstruction &d, std::uint32_t pc) {
    if (d.kind == OpcodeKind::J || d.kind == OpcodeKind::Jal) return ((pc + 4u) & 0xF0000000u) | (d.target << 2u);
    return pc + 4u + static_cast<std::uint32_t>(static_cast<std::int32_t>(d.immediate) * 4);
}

// Instructions whose semantics neither read nor write a general register:
// single-precision and VFPU arithmetic, conversions and prefixes. Run through
// the shared path, they need the cached registers neither stored before nor
// read back after.
bool touches_gprs(OpcodeKind kind) {
    switch (kind) {
    case OpcodeKind::AddS:
    case OpcodeKind::SubS:
    case OpcodeKind::MulS:
    case OpcodeKind::DivS:
    case OpcodeKind::SqrtS:
    case OpcodeKind::AbsS:
    case OpcodeKind::MovS:
    case OpcodeKind::NegS:
    case OpcodeKind::RoundWS:
    case OpcodeKind::TruncWS:
    case OpcodeKind::CeilWS:
    case OpcodeKind::FloorWS:
    case OpcodeKind::CvtWS:
    case OpcodeKind::CvtSW:
    case OpcodeKind::FpuCompare:
    case OpcodeKind::Vpfx:
    case OpcodeKind::Viim:
    case OpcodeKind::Vfim:
    case OpcodeKind::Vh2f:
    case OpcodeKind::Vf2h:
    case OpcodeKind::Vf2i:
    case OpcodeKind::Vi2f:
    case OpcodeKind::Vx2i:
    case OpcodeKind::VmidT:
    case OpcodeKind::Vmmov:
    case OpcodeKind::VfpuMatrixInit:
    case OpcodeKind::Vidt:
    case OpcodeKind::Vcst:
    case OpcodeKind::Vocp:
    case OpcodeKind::VfpuHorizontal:
    case OpcodeKind::Vrot:
    case OpcodeKind::Vtfm:
    case OpcodeKind::VfpuVectorInit:
    case OpcodeKind::VfpuVec3:
    case OpcodeKind::Vscl:
    case OpcodeKind::Vdot:
    case OpcodeKind::Vhdp:
    case OpcodeKind::Vcmp:
    case OpcodeKind::Vminmax:
    case OpcodeKind::VfpuCompare3:
    case OpcodeKind::Vcmov:
    case OpcodeKind::VfpuUnary:
    case OpcodeKind::Vmmul:
    case OpcodeKind::Vmscl:
    case OpcodeKind::VcrossQuat:
    case OpcodeKind::Vsgn:
    case OpcodeKind::Vsocp:
    case OpcodeKind::Vflush:
        return false;
    default:
        return true;
    }
}

void call_instruction_hook(Runtime *runtime, AllegrexContext *ctx, std::uint32_t address) {
    if (g_instruction_hook == nullptr) return;
    try {
        g_instruction_hook(*runtime, *ctx, address);
    } catch (const std::exception &error) {
        fail(runtime, error.what());
    } catch (...) {
        fail(runtime, "an unknown error");
    }
}

class Function {
public:
    Function(Runtime &runtime, std::uint32_t entry, const std::set<std::uint32_t> &hooked)
        : runtime_(runtime), memory_(runtime.memory()), entry_(entry), window_(entry & ~(kWindow - 1u)),
          hooked_(hooked) {}

    // The instructions reachable from the entry. False when the code cannot be
    // compiled here (not in memory, or a jump in a delay slot).
    bool discover() {
        std::vector<std::uint32_t> pending{entry_};
        while (!pending.empty()) {
            const std::uint32_t pc = pending.back();
            pending.pop_back();
            if (nodes_.contains(pc) || !inside(pc)) continue;
            if (nodes_.size() >= kMaxInstructions) return !nodes_.empty();
            Node node;
            node.instruction = decode_allegrex(memory_.aot_load32(pc));
            if (node.instruction.has_delay_slot()) {
                if (!memory_.contains(pc + 4u, 4u)) return false;
                node.slot = decode_allegrex(memory_.aot_load32(pc + 4u));
                if (node.slot.is_control_flow()) {
                    // A jump in a delay slot: the interpreter reports it. The
                    // function leaves to it rather than compiling it.
                    if (pc == entry_) return false;
                    cut_.insert(pc);
                    nodes_.emplace(pc, std::move(node));
                    continue;
                }
                const OpcodeKind kind = node.instruction.kind;
                if (kind != OpcodeKind::Jal && kind != OpcodeKind::Jr && kind != OpcodeKind::Jalr) {
                    pending.push_back(branch_target(node.instruction, pc));
                    if (is_conditional(kind)) pending.push_back(pc + 8u);
                }
            } else {
                pending.push_back(pc + 4u);
            }
            nodes_.emplace(pc, std::move(node));
        }
        return nodes_.contains(entry_);
    }

    // Why asmjit refused the last function.
    struct Errors : ErrorHandler {
        std::string first;
        void handle_error(asmjit::Error error, const char *message, BaseEmitter *) override {
            if (first.empty()) first = std::string(message) + " (" + std::to_string(static_cast<unsigned>(error)) + ")";
        }
    };

    // The compiled code, or null when the emitter failed.
    void *compile(JitRuntime &jit, Compiled &out) {
        Emitter &emitter = emitter_.emplace();
        emitter.code.init(jit.environment());
        emitter.code.set_error_handler(&errors_);
        if (settings().log) emitter.code.set_logger(&emitter.logger);
        ujit::BackendCompiler &cc = emitter.cc.emplace(&emitter.code);
        UniCompiler &uc = emitter.uc.emplace(&cc, CpuInfo::host().features(), asmjit::CpuHints{});
        uc.init_vec_width(ujit::VecWidth::k128);
        uc_ = &uc;

        FuncNode *function = uc.add_func(FuncSignature::build<void, void *, void *>());
        runtime_reg_ = uc.new_gpz("runtime");
        ctx_ = uc.new_gpz("ctx");
        ram_ = uc.new_gpz("ram");
        function->set_arg(0, runtime_reg_);
        function->set_arg(1, ctx_);
        uc.mov(ram_, Imm(reinterpret_cast<std::uint64_t>(memory_.ram_base())));
        fault_ = uc.new_label();
        leave_ = uc.new_label();
        choose_cached();
        for (std::uint32_t r = 1u; r < 32u; ++r) {
            if ((cached_ & (1u << r)) == 0u) continue;
            guest_[r] = uc.new_gp64();
            uc.load_u32(guest_[r].r32(), gpr(r));
        }
        {
            // A fault belongs to the call that met it.
            Gp flag = value();
            Gp zero = value();
            uc.mov(flag, Imm(reinterpret_cast<std::uint64_t>(&g_fault)));
            uc.mov(zero.r32(), Imm(0));
            uc.store_u8(ujit::mem_ptr(flag), zero.r32());
        }

        for (const auto &[pc, node] : nodes_) labels_[pc] = uc.new_label();
        // The code is laid out by address; the entry need not come first.
        if (nodes_.begin()->first != entry_) uc.j(labels_[entry_]);
        for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
            const std::uint32_t pc = it->first;
            uc.bind(labels_[pc]);
            if (hooked_.contains(pc)) emit_hook(pc);
            if (cut_.contains(pc)) {
                exit_to(pc); // a jump in a delay slot: the interpreter's business
                continue;
            }
            if (it->second.instruction.has_delay_slot()) {
                emit_branch(it->second, pc, out);
                continue;
            }
            emit_simple(it->second.instruction, pc, out);
            const auto next = std::next(it);
            if (next == nodes_.end() || next->first != pc + 4u) go(pc + 4u);
        }
        // A helper that met an error left the context as it was before the
        // call: the cached registers are newer.
        uc.bind(fault_);
        flush();
        uc.ret();
        // A helper that ran with the context up to date and may have changed
        // it: the context is newer.
        uc.bind(leave_);
        uc.ret();
        uc.end_func();
        if (const asmjit::Error error = cc.finalize(); error != asmjit::Error::kOk) {
            if (errors_.first.empty())
                errors_.first = "finalize failed (" + std::to_string(static_cast<unsigned>(error)) + ")";
            return nullptr;
        }
        void *result = nullptr;
        if (const asmjit::Error error = jit.add(&result, &emitter.code); error != asmjit::Error::kOk) {
            errors_.first = "no executable memory (" + std::to_string(static_cast<unsigned>(error)) + ")";
            return nullptr;
        }
        if (settings().log)
            std::cerr << "[jit] " << hex32(entry_) << ": " << nodes_.size() << " instructions\n"
                      << emitter.logger.data();
        out.low = nodes_.begin()->first;
        out.high = std::prev(nodes_.end())->first + 8u;
        out.source.resize(out.high - out.low);
        if (memory_.contains(out.low, out.high - out.low)) memory_.copy_out(out.low, out.source);
        return result;
    }

    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] const std::string &error() const noexcept { return errors_.first; }
    [[nodiscard]] std::size_t shared() const noexcept { return shared_; }

private:
    bool inside(std::uint32_t pc) const {
        if ((pc & ~(kWindow - 1u)) != window_ || !memory_.contains(pc, 4u)) return false;
        // Something else is registered there: an import, a host function or
        // a generated unit. Control goes to it through the dispatcher.
        return pc == entry_ || !runtime_.has_function(pc);
    }

    // --- Guest registers ---
    // Every register an instruction of the function may name is kept in a
    // host register (more than it uses is harmless); those it may write are
    // stored back. Instructions run through the interpreter's semantics may
    // write any register, but they read and write the context, which is up to
    // date around them and read back after.
    void choose_cached() {
        cached_ = 0u;
        written_ = 0u;
        if (!settings().cache_registers) return;
        const auto note = [this](const DecodedInstruction &d) {
            cached_ |= (1u << d.rs) | (1u << d.rt) | (1u << d.rd);
            written_ |= (1u << d.rt) | (1u << d.rd);
            if (semantics::is_link_branch(d.kind) || d.kind == OpcodeKind::Jal) written_ |= 1u << 31u;
        };
        for (const auto &[pc, node] : nodes_) {
            note(node.instruction);
            if (node.instruction.has_delay_slot()) note(node.slot);
        }
        written_ |= cached_ & (1u << 31u);
        cached_ = (cached_ | written_) & ~1u;
        written_ &= cached_;
    }
    bool cached(std::uint32_t r) const { return r != 0u && (cached_ & (1u << r)) != 0u; }
    // The context gets the cached registers the function may have changed.
    void flush() {
        for (std::uint32_t r = 1u; r < 32u; ++r)
            if ((written_ & (1u << r)) != 0u) uc_->store_u32(gpr(r), guest_[r].r32());
    }
    // The cached registers get the context's, after host code changed it.
    void reload() {
        for (std::uint32_t r = 1u; r < 32u; ++r)
            if (cached(r)) uc_->load_u32(guest_[r].r32(), gpr(r));
    }

    // --- Context access ---
    Mem gpr(std::uint32_t r) const {
        return ujit::mem_ptr(ctx_, static_cast<std::int32_t>(offsetof(AllegrexContext, gpr) + 4u * r));
    }
    Mem fpr(std::uint32_t r) const { return field(offsetof(AllegrexContext, fpr) + 4u * r); }
    Mem field(std::size_t offset) const { return ujit::mem_ptr(ctx_, static_cast<std::int32_t>(offset)); }
    Gp value() { return uc_->new_gp64(); }
    // A copy of a guest register, free to change.
    Gp read(std::uint32_t r) {
        Gp v = value();
        if (r == 0u)
            uc_->mov(v.r32(), Imm(0));
        else if (cached(r))
            uc_->mov(v.r32(), guest_[r].r32());
        else
            uc_->load_u32(v.r32(), gpr(r));
        return v;
    }
    void write(std::uint32_t r, const Gp &v) {
        if (r == 0u) return;
        if (cached(r))
            uc_->mov(guest_[r].r32(), v.r32());
        else
            uc_->store_u32(gpr(r), v.r32());
    }
    void write_imm(std::uint32_t r, std::uint32_t immediate) {
        if (r == 0u) return;
        Gp v = value();
        uc_->mov(v.r32(), Imm(immediate));
        write(r, v);
    }

    // --- Leaving ---
    void exit_to(std::uint32_t pc) {
        flush();
        Gp v = value();
        uc_->mov(v.r32(), Imm(pc));
        uc_->store_u32(field(offsetof(AllegrexContext, pc)), v.r32());
        uc_->ret();
    }
    void exit_to(const Gp &target) {
        flush();
        uc_->store_u32(field(offsetof(AllegrexContext, pc)), target.r32());
        uc_->ret();
    }
    // To another instruction of this function, or out.
    void go(std::uint32_t pc) {
        if (const auto found = labels_.find(pc); found != labels_.end())
            uc_->j(found->second);
        else
            exit_to(pc);
    }
    // A branch taken from `from` to `pc`: one back (a loop goes round) counts
    // down, and every kBackEdges of them the code leaves to the dispatcher at
    // `pc`, so that a loop that never calls out still lets the scheduler
    // switch threads, as the interpreter's bounded runs do.
    void go_from(std::uint32_t from, std::uint32_t pc) {
        const auto found = labels_.find(pc);
        if (found == labels_.end() || pc > from || !settings().preempt) {
            go(pc);
            return;
        }
        Gp at = value();
        Gp left = value();
        uc_->mov(at, Imm(reinterpret_cast<std::uint64_t>(&g_back_edges)));
        uc_->load_u32(left.r32(), ujit::mem_ptr(at));
        uc_->sub(left.r32(), left.r32(), Imm(1));
        uc_->store_u32(ujit::mem_ptr(at), left.r32());
        uc_->j(found->second, ujit::test_nz(left.r32(), left.r32()));
        uc_->mov(left.r32(), Imm(kBackEdges));
        uc_->store_u32(ujit::mem_ptr(at), left.r32());
        exit_to(pc);
    }
    void check_fault(const Label &to) {
        Gp flag = value();
        uc_->mov(flag, Imm(reinterpret_cast<std::uint64_t>(&g_fault)));
        uc_->load_u8(flag.r32(), ujit::mem_ptr(flag));
        uc_->j(to, ujit::test_nz(flag.r32(), flag.r32()));
    }
    void check_fault() { check_fault(fault_); }

    // Through a register: the host's functions may be further from the
    // compiled code than a direct call reaches (128 MiB on AArch64).
    template <typename Fn> InvokeNode *call(Fn *fn, const FuncSignature &signature) {
        InvokeNode *node = nullptr;
        Gp target = value();
        uc_->mov(target, Imm(reinterpret_cast<std::uint64_t>(fn)));
        uc_->cc->invoke(Out(node), target, signature);
        return node;
    }

    // An instruction hook: ctx.pc names the instruction, then the host runs,
    // with the context up to date; it may change the registers.
    void emit_hook(std::uint32_t pc) {
        flush();
        Gp v = value();
        uc_->mov(v.r32(), Imm(pc));
        uc_->store_u32(field(offsetof(AllegrexContext, pc)), v.r32());
        InvokeNode *node = call(&call_instruction_hook, FuncSignature::build<void, void *, void *, std::uint32_t>());
        node->set_arg(0, runtime_reg_);
        node->set_arg(1, ctx_);
        node->set_arg(2, v.r32());
        check_fault(leave_);
        reload();
    }

    // --- Instructions ---
    void emit_shared(const DecodedInstruction &d, std::uint32_t pc, Compiled &out) {
        out.decoded.push_back(std::make_unique<DecodedInstruction>(d));
        const DecodedInstruction *stored = out.decoded.back().get();
        const bool registers = touches_gprs(d.kind);
        if (registers) flush();
        Gp ptr = value();
        Gp pc_reg = value();
        Gp result = value();
        uc_->mov(ptr, Imm(reinterpret_cast<std::uint64_t>(stored)));
        uc_->mov(pc_reg.r32(), Imm(pc));
        InvokeNode *node = call(&run_shared,
            FuncSignature::build<std::uint32_t, void *, void *, const DecodedInstruction *, std::uint32_t>());
        node->set_arg(0, runtime_reg_);
        node->set_arg(1, ctx_);
        node->set_arg(2, ptr);
        node->set_arg(3, pc_reg.r32());
        node->set_ret(0, result.r32());
        // Stopped or retried: the context must be complete when this returns.
        uc_->j(registers ? leave_ : fault_, ujit::test_z(result.r32(), result.r32()));
        if (registers) reload();
        ++shared_;
    }

    // --- Single precision ---
    Vec load_float(std::uint32_t r) {
        Vec v = uc_->new_vec128_f32x1();
        uc_->v_loadu32_f32(v, fpr(r));
        return v;
    }
    // The low lane of a compare's mask as 0 or all ones.
    Gp mask_of(const Vec &mask) {
        Gp m = value();
        uc_->s_mov_u32(m.r32(), mask);
        return m;
    }
    // From an ordered compare's mask, all ones where it was unordered. (The
    // universal compiler has no scalar unordered compare on AArch64.)
    Gp unordered(const Vec &ordered_mask) {
        Gp m = mask_of(ordered_mask);
        uc_->not_(m.r32(), m.r32());
        return m;
    }
    // fd = fs op ft. Which NaN comes out of an operation on NaNs depends on
    // the host and on the order the interpreter's compiler put the operands
    // in, and infinity times zero gives the Allegrex's own NaN: a NaN result
    // goes through the interpreter's semantics, so both always agree.
    template <typename Emit>
    void float_operation(const DecodedInstruction &d, std::uint32_t pc, Compiled &out, bool binary, Emit emit) {
        UniCompiler &uc = *uc_;
        Vec a = load_float(d.rd);
        Vec b = binary ? load_float(d.rt) : a;
        emit(a, b);
        Vec ordered = uc.new_vec128_f32x1();
        uc.s_cmp_ord_f32(ordered, a, a);
        Gp nan = unordered(ordered);
        Label special = uc.new_label();
        Label done = uc.new_label();
        uc.j(special, ujit::test_nz(nan.r32(), nan.r32()));
        uc.v_storeu32_f32(fpr(d.sa), a);
        uc.j(done);
        uc.bind(special);
        emit_shared(d, pc, out);
        uc.bind(done);
    }
    bool emit_float(const DecodedInstruction &d, std::uint32_t pc, Compiled &out) {
        UniCompiler &uc = *uc_;
        switch (d.kind) {
        case OpcodeKind::AddS:
            float_operation(d, pc, out, true, [&](const Vec &a, const Vec &b) { uc.s_add_f32(a, a, b); });
            return true;
        case OpcodeKind::SubS:
            float_operation(d, pc, out, true, [&](const Vec &a, const Vec &b) { uc.s_sub_f32(a, a, b); });
            return true;
        case OpcodeKind::MulS:
            float_operation(d, pc, out, true, [&](const Vec &a, const Vec &b) { uc.s_mul_f32(a, a, b); });
            return true;
        case OpcodeKind::DivS:
            float_operation(d, pc, out, true, [&](const Vec &a, const Vec &b) { uc.s_div_f32(a, a, b); });
            return true;
        case OpcodeKind::SqrtS:
            float_operation(d, pc, out, false, [&](const Vec &a, const Vec &) { uc.s_sqrt_f32(a, a); });
            return true;
        case OpcodeKind::MovS:
        case OpcodeKind::AbsS:
        case OpcodeKind::NegS: {
            Gp v = value();
            uc.load_u32(v.r32(), fpr(d.rd));
            if (d.kind == OpcodeKind::AbsS) uc.and_(v.r32(), v.r32(), Imm(0x7FFFFFFFu));
            if (d.kind == OpcodeKind::NegS) uc.xor_(v.r32(), v.r32(), Imm(0x80000000u));
            uc.store_u32(fpr(d.sa), v.r32());
            return true;
        }
        case OpcodeKind::CvtSW: {
            Gp v = value();
            uc.load_u32(v.r32(), fpr(d.rd));
            Vec f = uc.new_vec128_f32x1();
            uc.s_cvt_int_to_f32(f, v.r32());
            uc.v_storeu32_f32(fpr(d.sa), f);
            return true;
        }
        case OpcodeKind::FpuCompare: {
            // Bit 0 of the condition: true when unordered; bit 1: equal;
            // bit 2: less than (ordered); both: less or equal.
            const std::uint32_t condition = d.word & 7u;
            Gp result = value();
            uc.mov(result.r32(), Imm(0));
            if (condition != 0u) {
                Vec a = load_float(d.rd);
                Vec b = load_float(d.rt);
                Vec mask = uc.new_vec128_f32x1();
                if ((condition & 1u) != 0u) {
                    uc.s_cmp_ord_f32(mask, a, b);
                    uc.or_(result.r32(), result.r32(), unordered(mask).r32());
                }
                if ((condition & 6u) != 0u) {
                    if ((condition & 6u) == 6u)
                        uc.s_cmp_le_f32(mask, a, b);
                    else if ((condition & 4u) != 0u)
                        uc.s_cmp_lt_f32(mask, a, b);
                    else
                        uc.s_cmp_eq_f32(mask, a, b);
                    uc.or_(result.r32(), result.r32(), mask_of(mask).r32());
                }
            }
            Gp fcr31 = value();
            uc.load_u32(fcr31.r32(), field(offsetof(AllegrexContext, fcr31)));
            uc.and_(fcr31.r32(), fcr31.r32(), Imm(~(1u << 23u)));
            uc.and_(result.r32(), result.r32(), Imm(1u << 23u));
            uc.or_(fcr31.r32(), fcr31.r32(), result.r32());
            uc.store_u32(field(offsetof(AllegrexContext, fcr31)), fcr31.r32());
            return true;
        }
        default:
            return false;
        }
    }

    // The guest address rs + imm, and its offset into RAM.
    Gp address(const DecodedInstruction &d, std::int32_t extra = 0) {
        Gp a = read(d.rs);
        const auto offset = static_cast<std::int32_t>(d.immediate) + extra;
        if (offset != 0) uc_->add(a.r32(), a.r32(), Imm(offset));
        return a;
    }
    Gp ram_offset(const Gp &a) {
        Gp o = value();
        uc_->and_(o.r32(), a.r32(), Imm(0x1FFFFFFFu));
        uc_->sub(o.r32(), o.r32(), Imm(GuestMemory::kPhysicalBase));
        return o;
    }

    enum class Load { U8, S8, U16, S16, U32 };
    Gp load(const Gp &a, Load kind) {
        const std::uint32_t bytes = kind == Load::U32 ? 4u : (kind == Load::U16 || kind == Load::S16) ? 2u : 1u;
        Gp v = value();
        Gp o = ram_offset(a);
        Label slow = uc_->new_label();
        Label done = uc_->new_label();
        uc_->j(slow, ujit::ucmp_gt(o.r32(), Imm(memory_.ram_limit(bytes))));
        const Mem at = ujit::mem_ptr(ram_, o.r64());
        switch (kind) {
        case Load::U8:
            uc_->load_u8(v.r32(), at);
            break;
        case Load::S8:
            uc_->load_i8(v.r32(), at);
            break;
        case Load::U16:
            uc_->load_u16(v.r32(), at);
            break;
        case Load::S16:
            uc_->load_i16(v.r32(), at);
            break;
        case Load::U32:
            uc_->load_u32(v.r32(), at);
            break;
        }
        uc_->j(done);
        uc_->bind(slow);
        InvokeNode *node = nullptr;
        const FuncSignature signature = FuncSignature::build<std::uint32_t, void *, std::uint32_t, std::uint32_t>();
        switch (kind) {
        case Load::U8:
            node = call(&load8_slow, signature);
            break;
        case Load::S8:
            node = call(&load8s_slow, signature);
            break;
        case Load::U16:
            node = call(&load16_slow, signature);
            break;
        case Load::S16:
            node = call(&load16s_slow, signature);
            break;
        case Load::U32:
            node = call(&load32_slow, signature);
            break;
        }
        node->set_arg(0, runtime_reg_);
        node->set_arg(1, a.r32());
        node->set_arg(2, Imm(pc_));
        node->set_ret(0, v.r32());
        check_fault();
        uc_->bind(done);
        return v;
    }

    void store(const Gp &a, const Gp &v, std::uint32_t bytes) {
        Label slow = uc_->new_label();
        Label done = uc_->new_label();
        // The write watch is fixed when memory is created: with it on, every
        // store takes the slow path, as in the generated code.
        if (!*memory_.write_watch_flag()) {
            Gp o = ram_offset(a);
            uc_->j(slow, ujit::ucmp_gt(o.r32(), Imm(memory_.ram_limit(bytes))));
            const Mem at = ujit::mem_ptr(ram_, o.r64());
            if (bytes == 1u)
                uc_->store_u8(at, v.r32());
            else if (bytes == 2u)
                uc_->store_u16(at, v.r32());
            else
                uc_->store_u32(at, v.r32());
            uc_->j(done);
        }
        uc_->bind(slow);
        const FuncSignature signature =
            FuncSignature::build<void, void *, std::uint32_t, std::uint32_t, std::uint32_t>();
        InvokeNode *node = bytes == 1u ? call(&store8_slow, signature)
            : bytes == 2u              ? call(&store16_slow, signature)
                                       : call(&store32_slow, signature);
        node->set_arg(0, runtime_reg_);
        node->set_arg(1, a.r32());
        node->set_arg(2, v.r32());
        node->set_arg(3, Imm(pc_));
        check_fault();
        uc_->bind(done);
    }

    void set_condition(std::uint32_t r, const UniCondition &condition) {
        Gp v = value();
        Gp one = value();
        uc_->mov(v.r32(), Imm(0));
        uc_->mov(one.r32(), Imm(1));
        uc_->cmov(v.r32(), one.r32(), condition);
        write(r, v);
    }

    void emit_simple(const DecodedInstruction &d, std::uint32_t pc, Compiled &out) {
        pc_ = pc;
        const auto imm = static_cast<std::int32_t>(d.immediate);
        const auto uimm = static_cast<std::uint32_t>(static_cast<std::uint16_t>(d.immediate));
        UniCompiler &uc = *uc_;
        switch (d.kind) {
        case OpcodeKind::Nop:
            return;
        case OpcodeKind::Cache:
            // An instruction cache line invalidated: the shared definition
            // drops compiled code whose bytes changed there, and this code,
            // which may be among it, leaves to run what follows as it is now.
            // In a delay slot it leaves with its branch.
            if (!semantics::invalidates_instructions(d)) return;
            emit_shared(d, pc, out);
            if (!in_slot_) exit_to(pc + 4u);
            return;
        case OpcodeKind::Addiu: {
            Gp v = read(d.rs);
            uc.add(v.r32(), v.r32(), Imm(imm));
            write(d.rt, v);
            return;
        }
        case OpcodeKind::Slti: {
            Gp v = read(d.rs);
            set_condition(d.rt, ujit::scmp_lt(v.r32(), Imm(imm)));
            return;
        }
        case OpcodeKind::Sltiu: {
            Gp v = read(d.rs);
            Gp bound = value();
            uc.mov(bound.r32(), Imm(static_cast<std::uint32_t>(imm)));
            set_condition(d.rt, ujit::ucmp_lt(v.r32(), bound.r32()));
            return;
        }
        case OpcodeKind::Andi:
        case OpcodeKind::Ori:
        case OpcodeKind::Xori: {
            Gp v = read(d.rs);
            if (d.kind == OpcodeKind::Andi)
                uc.and_(v.r32(), v.r32(), Imm(uimm));
            else if (d.kind == OpcodeKind::Ori)
                uc.or_(v.r32(), v.r32(), Imm(uimm));
            else
                uc.xor_(v.r32(), v.r32(), Imm(uimm));
            write(d.rt, v);
            return;
        }
        case OpcodeKind::Lui:
            write_imm(d.rt, uimm << 16u);
            return;
        case OpcodeKind::Addu:
        case OpcodeKind::Subu:
        case OpcodeKind::And:
        case OpcodeKind::Or:
        case OpcodeKind::Xor:
        case OpcodeKind::Nor:
        case OpcodeKind::Max:
        case OpcodeKind::Min: {
            Gp a = read(d.rs);
            Gp b = read(d.rt);
            switch (d.kind) {
            case OpcodeKind::Addu:
                uc.add(a.r32(), a.r32(), b.r32());
                break;
            case OpcodeKind::Subu:
                uc.sub(a.r32(), a.r32(), b.r32());
                break;
            case OpcodeKind::And:
                uc.and_(a.r32(), a.r32(), b.r32());
                break;
            case OpcodeKind::Or:
                uc.or_(a.r32(), a.r32(), b.r32());
                break;
            case OpcodeKind::Xor:
                uc.xor_(a.r32(), a.r32(), b.r32());
                break;
            case OpcodeKind::Nor:
                uc.or_(a.r32(), a.r32(), b.r32());
                uc.not_(a.r32(), a.r32());
                break;
            case OpcodeKind::Max:
                uc.smax(a.r32(), a.r32(), b.r32());
                break;
            default:
                uc.smin(a.r32(), a.r32(), b.r32());
                break;
            }
            write(d.rd, a);
            return;
        }
        case OpcodeKind::Slt:
        case OpcodeKind::Sltu: {
            Gp a = read(d.rs);
            Gp b = read(d.rt);
            set_condition(
                d.rd, d.kind == OpcodeKind::Slt ? ujit::scmp_lt(a.r32(), b.r32()) : ujit::ucmp_lt(a.r32(), b.r32()));
            return;
        }
        case OpcodeKind::Movz:
        case OpcodeKind::Movn: {
            if (d.rd == 0u) return;
            Gp source = read(d.rs);
            Gp test = read(d.rt);
            Gp v = read(d.rd);
            uc.cmov(v.r32(), source.r32(),
                d.kind == OpcodeKind::Movz ? ujit::cmp_eq(test.r32(), Imm(0)) : ujit::cmp_ne(test.r32(), Imm(0)));
            write(d.rd, v);
            return;
        }
        case OpcodeKind::Sll:
        case OpcodeKind::Srl:
        case OpcodeKind::Sra:
        case OpcodeKind::Rotr: {
            Gp v = read(d.rt);
            if (d.sa != 0u) {
                if (d.kind == OpcodeKind::Sll)
                    uc.shl(v.r32(), v.r32(), Imm(d.sa));
                else if (d.kind == OpcodeKind::Srl)
                    uc.shr(v.r32(), v.r32(), Imm(d.sa));
                else if (d.kind == OpcodeKind::Sra)
                    uc.sar(v.r32(), v.r32(), Imm(d.sa));
                else
                    uc.ror(v.r32(), v.r32(), Imm(d.sa));
            }
            write(d.rd, v);
            return;
        }
        case OpcodeKind::Sllv:
        case OpcodeKind::Srlv:
        case OpcodeKind::Srav:
        case OpcodeKind::Rotrv: {
            Gp v = read(d.rt);
            Gp s = read(d.rs);
            uc.and_(s.r32(), s.r32(), Imm(31));
            if (d.kind == OpcodeKind::Sllv)
                uc.shl(v.r32(), v.r32(), s.r32());
            else if (d.kind == OpcodeKind::Srlv)
                uc.shr(v.r32(), v.r32(), s.r32());
            else if (d.kind == OpcodeKind::Srav)
                uc.sar(v.r32(), v.r32(), s.r32());
            else
                uc.ror(v.r32(), v.r32(), s.r32());
            write(d.rd, v);
            return;
        }
        case OpcodeKind::Clz:
        case OpcodeKind::Clo: {
            Gp v = read(d.rs);
            if (d.kind == OpcodeKind::Clo) uc.not_(v.r32(), v.r32());
            // On x86 without LZCNT the count is BSR's, which leaves 0 with no
            // bit to find; the Allegrex counts 32 there, as AArch64 does.
            Gp count = value();
            Gp all = value();
            uc.clz(count.r32(), v.r32());
            uc.mov(all.r32(), Imm(32));
            uc.cmov(count.r32(), all.r32(), ujit::cmp_eq(v.r32(), Imm(0)));
            write(d.rd, count);
            return;
        }
        case OpcodeKind::Ext: {
            const std::uint32_t size = d.rd + 1u;
            Gp v = read(d.rs);
            if (d.sa != 0u) uc.shr(v.r32(), v.r32(), Imm(d.sa));
            if (size < 32u) uc.and_(v.r32(), v.r32(), Imm((1u << size) - 1u));
            write(d.rt, v);
            return;
        }
        case OpcodeKind::Ins: {
            const std::uint32_t size = d.rd >= d.sa ? d.rd - d.sa + 1u : 0u;
            const std::uint32_t source_mask = size >= 32u ? 0xFFFFFFFFu : (size == 0u ? 0u : ((1u << size) - 1u));
            const std::uint32_t destination_mask = source_mask << d.sa;
            Gp s = read(d.rs);
            Gp v = read(d.rt);
            uc.and_(s.r32(), s.r32(), Imm(source_mask));
            if (d.sa != 0u) uc.shl(s.r32(), s.r32(), Imm(d.sa));
            uc.and_(v.r32(), v.r32(), Imm(~destination_mask));
            uc.or_(v.r32(), v.r32(), s.r32());
            write(d.rt, v);
            return;
        }
        case OpcodeKind::Seb:
        case OpcodeKind::Seh: {
            const std::uint32_t shift = d.kind == OpcodeKind::Seb ? 24u : 16u;
            Gp v = read(d.rt);
            uc.shl(v.r32(), v.r32(), Imm(shift));
            uc.sar(v.r32(), v.r32(), Imm(shift));
            write(d.rd, v);
            return;
        }
        case OpcodeKind::Lw:
        case OpcodeKind::Lh:
        case OpcodeKind::Lhu:
        case OpcodeKind::Lb:
        case OpcodeKind::Lbu: {
            const Load kind = d.kind == OpcodeKind::Lw ? Load::U32
                : d.kind == OpcodeKind::Lh             ? Load::S16
                : d.kind == OpcodeKind::Lhu            ? Load::U16
                : d.kind == OpcodeKind::Lb             ? Load::S8
                                                       : Load::U8;
            Gp a = address(d);
            Gp v = load(a, kind);
            write(d.rt, v);
            return;
        }
        case OpcodeKind::Sw:
        case OpcodeKind::Sh:
        case OpcodeKind::Sb: {
            Gp a = address(d);
            Gp v = read(d.rt);
            store(a, v, d.kind == OpcodeKind::Sw ? 4u : d.kind == OpcodeKind::Sh ? 2u : 1u);
            return;
        }
        case OpcodeKind::Lwc1: {
            Gp a = address(d);
            Gp v = load(a, Load::U32);
            uc.store_u32(field(offsetof(AllegrexContext, fpr) + 4u * d.rt), v.r32());
            return;
        }
        case OpcodeKind::Swc1: {
            Gp a = address(d);
            Gp v = value();
            uc.load_u32(v.r32(), field(offsetof(AllegrexContext, fpr) + 4u * d.rt));
            store(a, v, 4u);
            return;
        }
        case OpcodeKind::Mfc1: {
            Gp v = value();
            uc.load_u32(v.r32(), field(offsetof(AllegrexContext, fpr) + 4u * d.rd));
            write(d.rt, v);
            return;
        }
        case OpcodeKind::Mtc1: {
            Gp v = read(d.rt);
            uc.store_u32(field(offsetof(AllegrexContext, fpr) + 4u * d.rd), v.r32());
            return;
        }
        case OpcodeKind::Mfhi:
        case OpcodeKind::Mflo: {
            Gp v = value();
            uc.load_u32(v.r32(),
                field(d.kind == OpcodeKind::Mfhi ? offsetof(AllegrexContext, hi) : offsetof(AllegrexContext, lo)));
            write(d.rd, v);
            return;
        }
        case OpcodeKind::Mthi:
        case OpcodeKind::Mtlo: {
            Gp v = read(d.rs);
            uc.store_u32(
                field(d.kind == OpcodeKind::Mthi ? offsetof(AllegrexContext, hi) : offsetof(AllegrexContext, lo)),
                v.r32());
            return;
        }
        case OpcodeKind::Mult:
        case OpcodeKind::Multu: {
            Gp a = read(d.rs);
            Gp b = read(d.rt);
            if (d.kind == OpcodeKind::Mult) {
                // Sign-extend both to 64 bits.
                uc.shl(a, a, Imm(32));
                uc.sar(a, a, Imm(32));
                uc.shl(b, b, Imm(32));
                uc.sar(b, b, Imm(32));
            }
            uc.mul(a, a, b);
            uc.store_u32(field(offsetof(AllegrexContext, lo)), a.r32());
            uc.shr(a, a, Imm(32));
            uc.store_u32(field(offsetof(AllegrexContext, hi)), a.r32());
            return;
        }
        default:
            if (settings().native_fpu && emit_float(d, pc, out)) return;
            emit_shared(d, pc, out);
            return;
        }
    }

    // A delay slot, with its instruction hook if it has one.
    void emit_slot(const Node &node, std::uint32_t pc, Compiled &out) {
        if (hooked_.contains(pc + 4u)) emit_hook(pc + 4u);
        in_slot_ = true;
        emit_simple(node.slot, pc + 4u, out);
        in_slot_ = false;
    }

    // A jump or branch with its delay slot. Operands are read before the slot
    // runs; the link register is written before it, as the interpreter does.
    void emit_branch(const Node &node, std::uint32_t pc, Compiled &out) {
        const DecodedInstruction &d = node.instruction;
        UniCompiler &uc = *uc_;
        const std::uint32_t return_pc = pc + 8u;
        switch (d.kind) {
        case OpcodeKind::J:
        case OpcodeKind::Jal: {
            const std::uint32_t target = branch_target(d, pc);
            if (d.kind == OpcodeKind::Jal) write_imm(31u, return_pc);
            emit_slot(node, pc, out);
            // A call leaves through the dispatcher, where a host function may
            // stand in for the callee.
            if (d.kind == OpcodeKind::Jal)
                exit_to(target);
            else
                go_from(pc, target);
            return;
        }
        case OpcodeKind::Jr:
        case OpcodeKind::Jalr: {
            Gp target = read(d.rs);
            if (hooked_.contains(pc + 4u)) {
                // A hook in the slot may change rs; the jump keeps what it read.
                Gp kept = value();
                uc.mov(kept.r32(), target.r32());
                target = kept;
            }
            if (d.kind == OpcodeKind::Jalr) write_imm(d.rd == 0u ? 31u : d.rd, return_pc);
            emit_slot(node, pc, out);
            exit_to(target);
            return;
        }
        default:
            break;
        }

        // Conditional: the operands as they are before the slot.
        Gp a = value();
        Gp b = value();
        bool test_bit = false; // condition is (a & mask) != 0 or == 0
        bool want_set = false;
        std::uint32_t mask = 0u;
        switch (d.kind) {
        case OpcodeKind::Bc1f:
        case OpcodeKind::Bc1t:
        case OpcodeKind::Bc1fl:
        case OpcodeKind::Bc1tl:
            uc.load_u32(a.r32(), field(offsetof(AllegrexContext, fcr31)));
            test_bit = true;
            mask = 1u << 23u;
            want_set = d.kind == OpcodeKind::Bc1t || d.kind == OpcodeKind::Bc1tl;
            break;
        case OpcodeKind::Bvf:
        case OpcodeKind::Bvt:
        case OpcodeKind::Bvfl:
        case OpcodeKind::Bvtl:
            uc.load_u32(a.r32(), field(offsetof(AllegrexContext, vfpu_ctrl) + 4u * 3u));
            test_bit = true;
            mask = 1u << ((d.word >> 18u) & 7u);
            want_set = d.kind == OpcodeKind::Bvt || d.kind == OpcodeKind::Bvtl;
            break;
        default:
            a = read(d.rs);
            b = read(d.rt);
            break;
        }
        const auto condition = [&]() -> UniCondition {
            if (test_bit) return want_set ? ujit::test_nz(a.r32(), Imm(mask)) : ujit::test_z(a.r32(), Imm(mask));
            switch (d.kind) {
            case OpcodeKind::Beq:
            case OpcodeKind::Beql:
                return ujit::cmp_eq(a.r32(), b.r32());
            case OpcodeKind::Bne:
            case OpcodeKind::Bnel:
                return ujit::cmp_ne(a.r32(), b.r32());
            case OpcodeKind::Blez:
            case OpcodeKind::Blezl:
                return ujit::scmp_le(a.r32(), Imm(0));
            case OpcodeKind::Bgtz:
            case OpcodeKind::Bgtzl:
                return ujit::scmp_gt(a.r32(), Imm(0));
            case OpcodeKind::Bltz:
            case OpcodeKind::Bltzl:
            case OpcodeKind::Bltzal:
            case OpcodeKind::Bltzall:
                return ujit::scmp_lt(a.r32(), Imm(0));
            default:
                return ujit::scmp_ge(a.r32(), Imm(0));
            }
        };
        if (semantics::is_link_branch(d.kind)) write_imm(31u, return_pc);
        const std::uint32_t target = branch_target(d, pc);
        Label taken = uc.new_label();
        if (semantics::is_likely_branch(d.kind)) {
            uc.j(taken, condition());
            go(return_pc); // not taken: the slot is annulled
            uc.bind(taken);
            emit_slot(node, pc, out);
            go_from(pc, target);
            return;
        }
        // The condition is taken from the saved operands after the slot.
        Gp flag = value();
        Gp one = value();
        uc.mov(flag.r32(), Imm(0));
        uc.mov(one.r32(), Imm(1));
        uc.cmov(flag.r32(), one.r32(), condition());
        emit_slot(node, pc, out);
        uc.j(taken, ujit::test_nz(flag.r32(), flag.r32()));
        go(return_pc);
        uc.bind(taken);
        go_from(pc, target);
    }

    Runtime &runtime_;
    GuestMemory &memory_;
    std::uint32_t entry_;
    std::uint32_t window_;
    const std::set<std::uint32_t> &hooked_;
    std::map<std::uint32_t, Node> nodes_;
    std::set<std::uint32_t> cut_; // branches with a jump in their slot
    std::map<std::uint32_t, Label> labels_;
    UniCompiler *uc_{};
    Gp runtime_reg_;
    Gp ctx_;
    Gp ram_;
    Label fault_;
    Label leave_;
    std::array<Gp, 32> guest_{};
    std::uint32_t cached_{};  // guest registers kept in host registers, a bit each
    std::uint32_t written_{}; // ... that the function may change
    std::size_t shared_{};
    std::uint32_t pc_{}; // the instruction being emitted, for the memory helpers' messages
    bool in_slot_{};     // emitting a delay slot
    Errors errors_;
    // The emitter of compile(), kept here so the helpers can reach it.
    struct Emitter {
        CodeHolder code;
        StringLogger logger;
        std::optional<ujit::BackendCompiler> cc;
        std::optional<UniCompiler> uc;
    };
    std::optional<Emitter> emitter_;
};

} // namespace

const Settings &settings() {
    static const Settings loaded = [] {
        Settings s;
        s.log = std::getenv("PSPRECOMP_JIT_LOG") != nullptr;
        s.trace = std::getenv("PSPRECOMP_JIT_TRACE") != nullptr;
        s.cache_registers = std::getenv("PSPRECOMP_JIT_NO_REGISTER_CACHE") == nullptr;
        s.native_fpu = std::getenv("PSPRECOMP_JIT_NO_NATIVE_FPU") == nullptr;
        s.preempt = std::getenv("PSPRECOMP_JIT_NO_PREEMPTION") == nullptr;
        return s;
    }();
    return loaded;
}

Compilation compile(Runtime &runtime, asmjit::JitRuntime &jit, std::uint32_t entry,
    const std::set<std::uint32_t> &hooked, Compiled &out) {
    Compilation result;
    Function function(runtime, entry, hooked);
    if (!function.discover()) return result;
    result.code = function.compile(jit, out);
    if (result.code == nullptr) {
        result.error = function.error();
        if (result.error.empty()) result.error = "the emitter failed";
        return result;
    }
    result.instructions = function.size();
    result.shared = function.shared();
    return result;
}

void set_instruction_hook(JitInstructionHook hook) noexcept {
    g_instruction_hook = hook;
}

} // namespace psprecomp::jit
