// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/frontend/A32/a32_types.h>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/optimization_flags.h>
#include <dynarmic/ir/terminal.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/arm/dynarmic/arm_exclusive_monitor.h"
#include "core/arm/dynarmic/arm_tick_counts.h"
#include "core/arm/exception_handler.h"
#include "core/core.h"
#include "core/core_timing.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

#ifndef SIGILL
constexpr u32 SIGILL = 4;
#endif

#ifndef SIGTRAP
constexpr u32 SIGTRAP = 5;
#endif

namespace Core {

// Luma ARM11 dump 2026-10-01 18:36: file 0x544D78 `ldrh r0, [r4]` while resolving
// the fourcc "blyt". r0 is the masked directory offset 0x746954, which lands
// past the mapped file on hardware. Azahar's linear heap often still has that
// page, so the load returns garbage. NLPP_EMU_NAME_WALK_ABORT=1 forces that
// offset on the first name lookup, then raises the same data abort when the
// vanilla load is still in place. The EngPatcher cave replaces that load, so
// the guarded binary takes the walker's not-found return instead.
constexpr VAddr kNameWalkAddPc = 0x00644D74;
constexpr VAddr kNameWalkLoadPc = 0x00644D78;
constexpr u32 kVanillaNameLdrh = 0xE1D400B0;
constexpr u32 kDumpNameOffset = 0x00746954;
// Luma 2026-10-01 19:58: mov r4, r0; ldr r0, [r0] with r0 == 0.
constexpr VAddr kMenuVtMovPc = 0x002EEDA4;
// Luma 2026-10-03 00:50: ldrb r0, [r6, #0x5f] at 0x003C6070 with r6 == 0 (FAR 0x5F).
// Only reached through the beq at 0x003C5FCC, taken when [r4 + 0x50] is 0. The
// guarded binary branches to a null check there instead of the load.
constexpr VAddr kPaneFlagBrPc = 0x003C5FCC;

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(ARM_Dynarmic& parent)
        : parent(parent), svc_context(parent.system), memory(parent.memory) {}
    ~DynarmicUserCallbacks() = default;

    static void SetActive(DynarmicUserCallbacks* callbacks) {
        active = callbacks;
    }

    static bool NameWalkAbortEnabled() {
        static const bool enabled = [] {
            const char* env = std::getenv("NLPP_EMU_NAME_WALK_ABORT");
            return env != nullptr && env[0] == '1';
        }();
        return enabled;
    }

    static bool MenuVtNullEnabled() {
        static const bool enabled = [] {
            const char* env = std::getenv("NLPP_EMU_MENU_VT");
            return env != nullptr && env[0] == '1';
        }();
        return enabled;
    }

    static bool PaneFlagNullEnabled() {
        static const bool enabled = [] {
            const char* env = std::getenv("NLPP_EMU_PANE_FLAG");
            return env != nullptr && env[0] == '1';
        }();
        return enabled;
    }

    std::optional<std::uint32_t> MemoryReadCode(VAddr vaddr) override {
        return memory.Read32OrNullopt(vaddr);
    }

    std::uint8_t MemoryRead8(VAddr vaddr) override {
        if (FaultIfUnmapped(vaddr, false)) {
            return 0;
        }
        return memory.Read8(vaddr);
    }
    std::uint16_t MemoryRead16(VAddr vaddr) override {
        if (FaultIfUnmapped(vaddr, false)) {
            return 0;
        }
        return memory.Read16(vaddr);
    }
    std::uint32_t MemoryRead32(VAddr vaddr) override {
        if (FaultIfUnmapped(vaddr, false)) {
            return 0;
        }
        return memory.Read32(vaddr);
    }
    std::uint64_t MemoryRead64(VAddr vaddr) override {
        if (FaultIfUnmapped(vaddr, false)) {
            return 0;
        }
        return memory.Read64(vaddr);
    }

    void MemoryWrite8(VAddr vaddr, std::uint8_t value) override {
        if (FaultIfUnmapped(vaddr, true)) {
            return;
        }
        memory.Write8(vaddr, value);
    }
    void MemoryWrite16(VAddr vaddr, std::uint16_t value) override {
        if (FaultIfUnmapped(vaddr, true)) {
            return;
        }
        memory.Write16(vaddr, value);
    }
    void MemoryWrite32(VAddr vaddr, std::uint32_t value) override {
        if (FaultIfUnmapped(vaddr, true)) {
            return;
        }
        memory.Write32(vaddr, value);
    }
    void MemoryWrite64(VAddr vaddr, std::uint64_t value) override {
        if (FaultIfUnmapped(vaddr, true)) {
            return;
        }
        memory.Write64(vaddr, value);
    }

    // Guest data access to a page the current process has not mapped.
    // Hardware takes a precise data abort (PC stays on the faulting ldr/str, FAR is the address).
    // HLE reads go through MemorySystem directly and are not aborted here.
    bool FaultIfUnmapped(VAddr vaddr, bool write) {
        if (Core::AreExceptionsIgnoredForSession()) {
            return false;
        }
        if (!parent.system.KernelRunning() || parent.jit == nullptr) {
            return false;
        }
        const auto process = parent.system.Kernel().GetCurrentProcess();
        if (!process) {
            return false;
        }
        if (parent.memory.IsValidVirtualAddress(*process, vaddr)) {
            return false;
        }
        // Luma3DS bit 31 aliases FCRAM and MMIO without a page-table entry.
        constexpr VAddr LumaAliasBit = 0x80000000u;
        if (vaddr & LumaAliasBit) {
            const PAddr paddr = vaddr & ~LumaAliasBit;
            if ((paddr & 0xF0000000) == Memory::FCRAM_PADDR) {
                return false;
            }
            if ((paddr & 0xF0000000) == 0x10000000 && paddr >= Memory::IO_AREA_PADDR) {
                return false;
            }
        }
        parent.data_abort_far = vaddr;
        parent.data_abort_write = write;
        parent.jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
        return true;
    }

    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override {
        return memory.WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override {
        return memory.WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override {
        return memory.WriteExclusive32(vaddr, value, expected);
    }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override {
        return memory.WriteExclusive64(vaddr, value, expected);
    }

    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        // Should never happen.
        UNREACHABLE_MSG("InterpeterFallback reached with pc = 0x{:08x}, code = 0x{:08x}, num = {}",
                        pc, MemoryReadCode(pc).value(), num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
        svc_context.CallSVC(swi);
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::UndefinedInstruction:
        case Dynarmic::A32::Exception::UnpredictableInstruction:
        case Dynarmic::A32::Exception::DecodeError:
        case Dynarmic::A32::Exception::NoExecuteFault:
            break;
        case Dynarmic::A32::Exception::Breakpoint:
#ifdef ENABLE_GDBSTUB
            if (GDBStub::IsConnected()) {
                parent.SetPC(pc);
                parent.ServeBreak(SIGTRAP);
                return;
            }
#endif
            break;
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadInstruction:
            return;
        }

        static constexpr auto ExceptionToString = [](Dynarmic::A32::Exception e) -> std::string {
            switch (e) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
                return "UndefinedInstruction";
            case Dynarmic::A32::Exception::UnpredictableInstruction:
                return "UnpredictableInstruction";
            case Dynarmic::A32::Exception::DecodeError:
                return "DecodeError";
            case Dynarmic::A32::Exception::NoExecuteFault:
                return "NoExecuteFault";
            case Dynarmic::A32::Exception::Breakpoint:
                return "Breakpoint";
            default:
                return fmt::format("Unknown({})", e);
            }
        };

        parent.SetPC(pc);
#ifdef ENABLE_GDBSTUB
        if (GDBStub::IsConnected()) {
            parent.ServeBreak(SIGILL);
        } else
#endif
        {
            Core::ExceptionType exc_type;
            switch (exception) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
            case Dynarmic::A32::Exception::UnpredictableInstruction:
            case Dynarmic::A32::Exception::DecodeError:
            case Dynarmic::A32::Exception::NoExecuteFault:
                exc_type = Core::ExceptionType::UndefinedInstruction;
                break;
            case Dynarmic::A32::Exception::Breakpoint:
                exc_type = Core::ExceptionType::Break;
                break;
            default:
                exc_type = Core::ExceptionType::UndefinedInstruction;
                break;
            }
            Core::LogException(parent.system, exc_type);
        }
    }

    void AddTicks(std::uint64_t ticks) override {
        parent.GetTimer().AddTicks(ticks);
    }
    std::uint64_t GetTicksRemaining() override {
        s64 ticks = parent.GetTimer().GetDowncount();
        return static_cast<u64>(ticks <= 0 ? 0 : ticks);
    }
    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }

    bool PreCodeReadHook(bool is_thumb, VAddr pc, Dynarmic::A32::IREmitter& ir) override {
        if (is_thumb || pc != kNameWalkLoadPc || !NameWalkAbortEnabled()) {
            return true;
        }
        const auto insn = memory.Read32OrNullopt(pc);
        // The EngPatcher cave replaces this load with a branch. Only the vanilla
        // halfword raises the dump's data abort.
        if (!insn || *insn != kVanillaNameLdrh) {
            return true;
        }
        ir.CallHostFunction(&AbortNameWalkLoad,
                            ir.ZeroExtendWordToLong(ir.GetRegister(Dynarmic::A32::Reg::R4)));
        ir.SetTerm(Dynarmic::IR::Term::ReturnToDispatch{});
        return false;
    }

    void PreCodeTranslationHook(bool is_thumb, VAddr pc, Dynarmic::A32::IREmitter& ir) override {
        if (!is_thumb && pc == kNameWalkAddPc && NameWalkAbortEnabled() && !name_walk_poisoned) {
            // Immediate, not a host-side SetReg: the JIT would restore the old r0
            // after the call. The host call drops this block so the next lookup
            // translates without the forced offset.
            ir.CallHostFunction(&ArmNameWalkPoison);
            ir.SetRegister(Dynarmic::A32::Reg::R0, ir.Imm32(kDumpNameOffset));
        }
        if (!is_thumb && pc == kMenuVtMovPc && MenuVtNullEnabled() && !menu_vt_nulled) {
            // One null object, matching the 19:58 dump's r6. Later calls stay real.
            ir.CallHostFunction(&ArmMenuVtNull);
            ir.SetRegister(Dynarmic::A32::Reg::R0, ir.Imm32(0));
        }
        if (!is_thumb && pc == kPaneFlagBrPc && PaneFlagNullEnabled() && !pane_flag_nulled) {
            // r6 is dead on the not-taken path (the epilogue pops it), so nulling it
            // there is harmless. The host call disarms only once the branch is taken.
            ir.CallHostFunction(&ArmPaneFlagNull,
                                ir.ZeroExtendWordToLong(ir.GetRegister(Dynarmic::A32::Reg::R4)));
            ir.SetRegister(Dynarmic::A32::Reg::R6, ir.Imm32(0));
        }
    }

    static void ArmPaneFlagNull(u64 r4) {
        auto* cb = active;
        if (cb == nullptr || pane_flag_nulled) {
            return;
        }
        const auto slot = cb->memory.Read32OrNullopt(static_cast<VAddr>(r4) + 0x50);
        if (!slot || *slot != 0) {
            return;
        }
        pane_flag_nulled = true;
        cb->parent.InvalidateCacheRange(kPaneFlagBrPc, 4);
        LOG_WARNING(Core, "NLPP pane-flag: one-shot null r6 before {:#010x}", kPaneFlagBrPc);
    }

    static void ArmNameWalkPoison() {
        auto* cb = active;
        if (cb == nullptr || name_walk_poisoned) {
            return;
        }
        name_walk_poisoned = true;
        cb->parent.InvalidateCacheRange(kNameWalkAddPc, 4);
        LOG_WARNING(Core, "NLPP name-walk: one-shot directory offset {:#010x}", kDumpNameOffset);
    }

    static void ArmMenuVtNull() {
        auto* cb = active;
        if (cb == nullptr || menu_vt_nulled) {
            return;
        }
        menu_vt_nulled = true;
        cb->parent.InvalidateCacheRange(kMenuVtMovPc, 4);
        LOG_WARNING(Core, "NLPP menu-vt: one-shot null object before {:#010x}", kMenuVtMovPc);
    }

    static void AbortNameWalkLoad(u64 far) {
        auto* cb = active;
        if (cb == nullptr) {
            return;
        }
        const u32 fault = static_cast<u32>(far);
        cb->parent.data_abort_far = fault;
        cb->parent.data_abort_write = false;
        cb->parent.SetPC(kNameWalkLoadPc);
        if (cb->parent.jit != nullptr) {
            cb->parent.jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
        }
        LOG_CRITICAL(Core,
                     "NLPP name-walk data abort (dump 2026-10-01 18:36) FAR {:#010x} "
                     "offset {:#010x} PC {:#010x}",
                     fault, kDumpNameOffset, kNameWalkLoadPc);
    }

    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
    static thread_local DynarmicUserCallbacks* active;
    static bool name_walk_poisoned;
    static bool menu_vt_nulled;
    static bool pane_flag_nulled;
};

thread_local DynarmicUserCallbacks* DynarmicUserCallbacks::active = nullptr;
bool DynarmicUserCallbacks::name_walk_poisoned = false;
bool DynarmicUserCallbacks::menu_vt_nulled = false;
bool DynarmicUserCallbacks::pane_flag_nulled = false;

ARM_Dynarmic::ARM_Dynarmic(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id_,
                           std::shared_ptr<Core::Timing::Timer> timer_,
                           Core::ExclusiveMonitor& exclusive_monitor_)
    : ARM_Interface(core_id_, timer_), system(system_), memory(memory_),
      cb(std::make_unique<DynarmicUserCallbacks>(*this)),
      exclusive_monitor{dynamic_cast<Core::DynarmicExclusiveMonitor&>(exclusive_monitor_)} {
    SetPageTable(memory.GetCurrentPageTable());
}

ARM_Dynarmic::~ARM_Dynarmic() = default;

MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::Run() {
    ASSERT(memory.GetCurrentPageTable() == current_page_table);
    MICROPROFILE_SCOPE(ARM_Jit);
    if (break_flag) [[unlikely]] {
        return;
    }

    DynarmicUserCallbacks::SetActive(cb.get());
    HandleHalt(jit->Run());
    DynarmicUserCallbacks::SetActive(nullptr);
}

void ARM_Dynarmic::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }

    DynarmicUserCallbacks::SetActive(cb.get());
    HandleHalt(jit->Step());
    DynarmicUserCallbacks::SetActive(nullptr);
}

void ARM_Dynarmic::HandleHalt(Dynarmic::HaltReason hr) {
    if (!Dynarmic::Has(hr, Dynarmic::HaltReason::MemoryAbort)) {
        return;
    }
    jit->ClearHalt(Dynarmic::HaltReason::MemoryAbort);
    const auto far = data_abort_far;
    const bool write = data_abort_write;
    data_abort_far.reset();
    data_abort_write = false;
    // Dynarmic rewound PC to the faulting instruction before returning.
    Core::LogException(system, Core::ExceptionType::DataAbort, far, write);
}

void ARM_Dynarmic::SetPC(u32 pc) {
    jit->Regs()[15] = pc;
}

u32 ARM_Dynarmic::GetPC() const {
    return jit->Regs()[15];
}

u32 ARM_Dynarmic::GetReg(int index) const {
    return jit->Regs()[index];
}

void ARM_Dynarmic::SetReg(int index, u32 value) {
    jit->Regs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPReg(int index) const {
    return jit->ExtRegs()[index];
}

void ARM_Dynarmic::SetVFPReg(int index, u32 value) {
    jit->ExtRegs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return jit->Fpscr();
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }

    return UINT_MAX;
}

void ARM_Dynarmic::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        jit->SetFpscr(value);
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_Dynarmic::GetCPSR() const {
    return jit->Cpsr();
}

void ARM_Dynarmic::SetCPSR(u32 cpsr) {
    jit->SetCpsr(cpsr);
}

u32 ARM_Dynarmic::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return cp15_state.cp15_thread_uprw;
    case CP15_THREAD_URO:
        return cp15_state.cp15_thread_uro;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }

    return 0;
}

void ARM_Dynarmic::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        cp15_state.cp15_thread_uprw = value;
        return;
    case CP15_THREAD_URO:
        cp15_state.cp15_thread_uro = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }
}

void ARM_Dynarmic::SaveContext(ThreadContext& ctx) {
    ctx.cpu_registers = jit->Regs();
    ctx.cpsr = jit->Cpsr();
    ctx.fpu_registers = jit->ExtRegs();
    ctx.fpscr = jit->Fpscr();
    ctx.fpexc = fpexc;
}

void ARM_Dynarmic::LoadContext(const ThreadContext& ctx) {
    jit->Regs() = ctx.cpu_registers;
    jit->SetCpsr(ctx.cpsr);
    jit->ExtRegs() = ctx.fpu_registers;
    jit->SetFpscr(ctx.fpscr);
    fpexc = ctx.fpexc;
}

void ARM_Dynarmic::PrepareReschedule() {
    if (jit->IsExecuting()) {
        jit->HaltExecution();
    }
}

void ARM_Dynarmic::ClearInstructionCache() {
    for (const auto& j : jits) {
        j.second->ClearCache();
    }
}

void ARM_Dynarmic::InvalidateCacheRange(u32 start_address, std::size_t length) {
    jit->InvalidateCacheRange(start_address, length);
}

void ARM_Dynarmic::ClearExclusiveState() {
    jit->ClearExclusiveState();
}

std::shared_ptr<Memory::PageTable> ARM_Dynarmic::GetPageTable() const {
    return current_page_table;
}

void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    current_page_table = page_table;
    ThreadContext ctx{};
    if (jit) {
        SaveContext(ctx);
    }

    auto iter = jits.find(current_page_table);
    if (iter != jits.end()) {
        jit = iter->second.get();
        LoadContext(ctx);
        return;
    }

    auto new_jit = MakeJit();
    jit = new_jit.get();
    LoadContext(ctx);
    jits.emplace(current_page_table, std::move(new_jit));
}

void ARM_Dynarmic::ServeBreak([[maybe_unused]] int signal) {
#ifdef ENABLE_GDBSTUB
    GDBStub::Break(signal);
#endif
}

std::unique_ptr<Dynarmic::A32::Jit> ARM_Dynarmic::MakeJit() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = cb.get();
    if (current_page_table) {
        config.page_table = &current_page_table->GetPointerArray();
    }
    config.coprocessors[15] = std::make_shared<DynarmicCP15>(cp15_state);
    config.define_unpredictable_behaviour = true;
    // After every guest load/store, leave the block if a callback raised MemoryAbort
    // and restore PC to that instruction. Without this, unmapped reads return 0 and
    // execution continues, so a hardware data abort never appears.
    config.check_halt_on_memory_access = true;

    // Multi-process state
    config.processor_id = GetID();
    config.global_monitor = &exclusive_monitor.monitor;

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

} // namespace Core
