// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitArm64/Jit.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Common/Align.h"
#include "Common/BitSet.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

#include "Core/HW/GPFifo.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitArm64/Jit_Util.h"
#include "Core/PowerPC/JitArmCommon/BackPatch.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"

using namespace Arm64Gen;

void JitArm64::DoBacktrace(uintptr_t access_address, SContext* ctx)
{
  for (int i = 0; i < 30; i += 2)
  {
    ERROR_LOG_FMT(DYNA_REC, "R{}: {:#018x}\tR{}: {:#018x}", i, ctx->CTX_REG(i), i + 1,
                  ctx->CTX_REG(i + 1));
  }

  ERROR_LOG_FMT(DYNA_REC, "R30: {:#018x}\tSP: {:#018x}", ctx->CTX_LR, ctx->CTX_SP);

  ERROR_LOG_FMT(DYNA_REC, "Access Address: {:#018x}", access_address);
  ERROR_LOG_FMT(DYNA_REC, "PC: {:#018x}", ctx->CTX_PC);

  ERROR_LOG_FMT(DYNA_REC, "Memory Around PC");

  std::string pc_memory;
  for (u64 pc = (ctx->CTX_PC - 32); pc < (ctx->CTX_PC + 32); pc += 16)
  {
    pc_memory += fmt::format("{:08x}{:08x}{:08x}{:08x}", Common::swap32(*(u32*)pc),
                             Common::swap32(*(u32*)(pc + 4)), Common::swap32(*(u32*)(pc + 8)),
                             Common::swap32(*(u32*)(pc + 12)));

    ERROR_LOG_FMT(DYNA_REC, "{:#018x}: {:08x} {:08x} {:08x} {:08x}", pc, *(u32*)pc, *(u32*)(pc + 4),
                  *(u32*)(pc + 8), *(u32*)(pc + 12));
  }

  ERROR_LOG_FMT(DYNA_REC, "Full block: {}", pc_memory);
}

void JitArm64::EmitBackpatchRoutine(u32 flags, MemAccessMode mode, ARM64Reg RS, ARM64Reg addr,
                                    BitSet32 gprs_to_push, BitSet32 fprs_to_push,
                                    bool emitting_routine)
{
  const u32 access_size = BackPatchInfo::GetFlagSize(flags);

  if (m_accurate_cpu_cache_enabled)
    mode = MemAccessMode::AlwaysSlowAccess;

  const bool emit_fast_access = mode != MemAccessMode::AlwaysSlowAccess;
  const bool emit_slow_access = mode != MemAccessMode::AlwaysFastAccess;

  bool in_far_code = false;
  const u8* fast_access_start = GetCodePtr();
  std::optional<FixupBranch> slow_access_fixup;

  if (emit_fast_access)
  {
    ARM64Reg memory_base = MEM_REG;
    ARM64Reg memory_offset = addr;

    if (!jo.fastmem)
    {
      const ARM64Reg temp = emitting_routine ? ARM64Reg::W3 : ARM64Reg::W30;

      memory_base = EncodeRegTo64(temp);
      memory_offset = ARM64Reg::W0;

      LSR(temp, addr, PowerPC::BAT_INDEX_SHIFT);
      LDR(memory_base, MEM_REG, ArithOption(temp, true));

      if (emit_slow_access)
      {
        FixupBranch pass = CBNZ(memory_base);
        slow_access_fixup = B();
        SetJumpTarget(pass);
      }

      AND(memory_offset, addr, LogicalImm(PowerPC::BAT_PAGE_SIZE - 1, GPRSize::B64));
    }
    else if (emit_slow_access && emitting_routine)
    {
      const ARM64Reg temp1 = flags & BackPatchInfo::FLAG_STORE ? ARM64Reg::W1 : ARM64Reg::W3;
      const ARM64Reg temp2 = ARM64Reg::W0;

      slow_access_fixup = CheckIfSafeAddress(addr, temp1, temp2);
    }

    if ((flags & BackPatchInfo::FLAG_STORE) && (flags & BackPatchInfo::FLAG_FLOAT))
    {
      ARM64Reg temp = ARM64Reg::D0;
      temp = ByteswapBeforeStore(this, &m_float_emit, temp, EncodeRegToDouble(RS), flags, true);

      m_float_emit.STR(access_size, temp, memory_base, memory_offset);
    }
    else if ((flags & BackPatchInfo::FLAG_LOAD) && (flags & BackPatchInfo::FLAG_FLOAT))
    {
      m_float_emit.LDR(access_size, EncodeRegToDouble(RS), memory_base, memory_offset);

      ByteswapAfterLoad(this, &m_float_emit, EncodeRegToDouble(RS), EncodeRegToDouble(RS), flags,
                        true, false);
    }
    else if (flags & BackPatchInfo::FLAG_STORE)
    {
      ARM64Reg temp = ARM64Reg::W1;
      temp = ByteswapBeforeStore(this, &m_float_emit, temp, RS, flags, true);

      if (flags & BackPatchInfo::FLAG_SIZE_32)
        STR(temp, memory_base, memory_offset);
      else if (flags & BackPatchInfo::FLAG_SIZE_16)
        STRH(temp, memory_base, memory_offset);
      else
        STRB(temp, memory_base, memory_offset);
    }
    else if (flags & BackPatchInfo::FLAG_ZERO_256)
    {
      // This literally only stores 32bytes of zeros to the target address
      ARM64Reg temp = ARM64Reg::X30;
      ADD(temp, memory_base, memory_offset);
      STP(IndexType::Signed, ARM64Reg::ZR, ARM64Reg::ZR, temp, 0);
      STP(IndexType::Signed, ARM64Reg::ZR, ARM64Reg::ZR, temp, 16);
    }
    else
    {
      if (flags & BackPatchInfo::FLAG_SIZE_32)
        LDR(RS, memory_base, memory_offset);
      else if (flags & BackPatchInfo::FLAG_SIZE_16)
        LDRH(RS, memory_base, memory_offset);
      else if (flags & BackPatchInfo::FLAG_SIZE_8)
        LDRB(RS, memory_base, memory_offset);

      ByteswapAfterLoad(this, &m_float_emit, RS, RS, flags, true, false);
    }
  }
  const u8* fast_access_end = GetCodePtr();

  if (emit_slow_access)
  {
    const bool memcheck = jo.memcheck && !emitting_routine;

    if (emit_fast_access)
    {
      in_far_code = true;
      SwitchToFarCode();

      if (jo.fastmem && !emitting_routine)
      {
        FastmemArea* fastmem_area = &m_fault_to_handler[fast_access_end];
        fastmem_area->fast_access_code = fast_access_start;
        fastmem_area->slow_access_code = GetCodePtr();
      }
    }

    if (slow_access_fixup)
      SetJumpTarget(*slow_access_fixup);

    // Stores whose address only turns out at run time to be the gather pipe (games keep its
    // address in a variable, and paired-single stores go through the routines in JitAsm.cpp)
    // come here, and through the MMU, for every word of FIFO data. Do the ones that don't
    // complete a 32-byte burst right here; the rest take the usual way, which also sends the
    // burst and marks the instruction for a gather pipe interrupt check. Only when the BATs map
    // 0xCC008000 to the gather pipe (changing them clears the JIT cache and regenerates the
    // routines), and only with MSR.DR set, which is checked here because the routines are shared.
    std::optional<FixupBranch> gather_pipe_done;
    if (!jo.memcheck && (flags & BackPatchInfo::FLAG_STORE) && !(flags & BackPatchInfo::FLAG_ZERO_256) &&
        access_size <= 64 && jo.optimizeGatherPipe && m_mmu.IsGatherPipeBATAddress(0xCC008000))
    {
      // Two registers that hold neither the address nor the value, saved around this.
      std::vector<ARM64Reg> temps;
      for (const ARM64Reg candidate :
           {ARM64Reg::X0, ARM64Reg::X1, ARM64Reg::X3, ARM64Reg::X4, ARM64Reg::X5})
      {
        const bool holds_value =
            !(flags & BackPatchInfo::FLAG_FLOAT) && DecodeReg(RS) == DecodeReg(candidate);
        if (!holds_value && DecodeReg(addr) != DecodeReg(candidate) && temps.size() < 2)
          temps.push_back(candidate);
      }
      const ARM64Reg pipe_ptr = temps[0];
      const ARM64Reg temp = temps[1];
      const u32 bytes = access_size / 8;

      STP(IndexType::Pre, pipe_ptr, temp, ARM64Reg::SP, -16);

      static_assert(GPFifo::GATHER_PIPE_PHYSICAL_ADDRESS == 0x0C008000);
      LSR(EncodeRegTo32(temp), EncodeRegTo32(addr), 12);
      SUB(EncodeRegTo32(temp), EncodeRegTo32(temp), 0xCC, true);
      CMP(EncodeRegTo32(temp), 0x008);
      FixupBranch not_gather_pipe = B(CC_NEQ);
      static_assert(UReg_MSR{}.DR.StartBit() == 4);
      LDR(IndexType::Unsigned, EncodeRegTo32(temp), PPC_REG, PPCSTATE_OFF(msr));
      FixupBranch real_mode = TBZ(EncodeRegTo32(temp), 4);

      static_assert(PPCSTATE_OFF(gather_pipe_ptr) <= 504);
      static_assert(PPCSTATE_OFF(gather_pipe_ptr) + 8 == PPCSTATE_OFF(gather_pipe_base_ptr));
      LDP(IndexType::Signed, pipe_ptr, temp, PPC_REG, PPCSTATE_OFF(gather_pipe_ptr));
      SUB(temp, pipe_ptr, temp);
      CMP(temp, GPFifo::GATHER_PIPE_SIZE - bytes);
      FixupBranch completes_burst = B(CC_GE);

      // The bytes as a store to RAM would leave them (as the fast access above writes them)
      if (flags & BackPatchInfo::FLAG_FLOAT)
      {
        const ARM64Reg value = ByteswapBeforeStore(this, &m_float_emit, ARM64Reg::D0,
                                                   EncodeRegToDouble(RS), flags, true);
        m_float_emit.STR(access_size, IndexType::Post, value, pipe_ptr, bytes);
      }
      else
      {
        const ARM64Reg value =
            ByteswapBeforeStore(this, &m_float_emit, EncodeRegTo32(temp), RS, flags, true);
        if (access_size == 32)
          STR(IndexType::Post, value, pipe_ptr, 4);
        else if (access_size == 16)
          STRH(IndexType::Post, value, pipe_ptr, 2);
        else
          STRB(IndexType::Post, value, pipe_ptr, 1);
      }
      STR(IndexType::Unsigned, pipe_ptr, PPC_REG, PPCSTATE_OFF(gather_pipe_ptr));
      LDP(IndexType::Post, pipe_ptr, temp, ARM64Reg::SP, 16);
      gather_pipe_done = B();

      SetJumpTarget(not_gather_pipe);
      SetJumpTarget(real_mode);
      SetJumpTarget(completes_burst);
      LDP(IndexType::Post, pipe_ptr, temp, ARM64Reg::SP, 16);
    }

    const ARM64Reg temp_gpr = ARM64Reg::W1;
    const int temp_gpr_index = DecodeReg(temp_gpr);

    BitSet32 gprs_to_push_early = {};
    if (memcheck)
      gprs_to_push_early[temp_gpr_index] = true;
    if (flags & BackPatchInfo::FLAG_LOAD)
      gprs_to_push_early[0] = true;

    // If we're already pushing one register in the first PushRegisters call, we can push a
    // second one for free. Let's do so, since it might save one instruction in the second
    // PushRegisters call. (Do not do this for caller-saved registers which may be in the register
    // cache, or WriteConditionalExceptionExit won't be able to flush the register cache correctly!)
    if ((gprs_to_push & gprs_to_push_early).Count() & 1)
      gprs_to_push_early[30] = true;

    ABI_PushRegisters(gprs_to_push & gprs_to_push_early);
    ABI_PushRegisters(gprs_to_push & ~gprs_to_push_early);
    m_float_emit.ABI_PushRegisters(fprs_to_push, ARM64Reg::X30);

    // PC is used by memory watchpoints (if enabled), profiling where to insert gather pipe
    // interrupt checks, and printing accurate PC locations in debug logs.
    //
    // In the case of JitAsm routines, we don't know the PC here,
    // so the caller has to store the PC themselves.
    if (!emitting_routine)
    {
      MOVI2R(ARM64Reg::W30, js.compilerPC);
      STR(IndexType::Unsigned, ARM64Reg::W30, PPC_REG, PPCSTATE_OFF(pc));
    }

    if (flags & BackPatchInfo::FLAG_STORE)
    {
      ARM64Reg src_reg = RS;
      const ARM64Reg dst_reg = access_size == 64 ? ARM64Reg::X1 : ARM64Reg::W1;

      if (flags & BackPatchInfo::FLAG_FLOAT)
      {
        if (access_size == 64)
          m_float_emit.FMOV(dst_reg, EncodeRegToDouble(RS));
        else
          m_float_emit.FMOV(dst_reg, EncodeRegToSingle(RS));

        src_reg = dst_reg;
      }

      if (flags & BackPatchInfo::FLAG_PAIR)
      {
        // Compensate for the Write_ functions swapping the whole write instead of each pair
        SwapPairs(this, dst_reg, src_reg, flags);
        src_reg = dst_reg;
      }

      const bool reverse = (flags & BackPatchInfo::FLAG_REVERSE) != 0;

      if (access_size == 64)
      {
        ABI_CallFunction(reverse ? &PowerPC::WriteU64SwapFromJit : &PowerPC::WriteU64FromJit,
                         &m_mmu, src_reg, ARM64Reg::W2);
      }
      else if (access_size == 32)
      {
        ABI_CallFunction(reverse ? &PowerPC::WriteU32SwapFromJit : &PowerPC::WriteU32FromJit,
                         &m_mmu, src_reg, ARM64Reg::W2);
      }
      else if (access_size == 16)
      {
        ABI_CallFunction(reverse ? &PowerPC::WriteU16SwapFromJit : &PowerPC::WriteU16FromJit,
                         &m_mmu, src_reg, ARM64Reg::W2);
      }
      else
      {
        ABI_CallFunction(&PowerPC::WriteU8FromJit, &m_mmu, src_reg, ARM64Reg::W2);
      }
    }
    else if (flags & BackPatchInfo::FLAG_ZERO_256)
    {
      ABI_CallFunction(&PowerPC::ClearDCacheLineFromJit, &m_mmu, ARM64Reg::W1);
    }
    else
    {
      if (access_size == 64)
        ABI_CallFunction(&PowerPC::ReadU64FromJit, &m_mmu, ARM64Reg::W1);
      else if (access_size == 32)
        ABI_CallFunction(&PowerPC::ReadU32FromJit, &m_mmu, ARM64Reg::W1);
      else if (access_size == 16)
        ABI_CallFunction(&PowerPC::ReadU16FromJit, &m_mmu, ARM64Reg::W1);
      else
        ABI_CallFunction(&PowerPC::ReadU8FromJit, &m_mmu, ARM64Reg::W1);
    }

    m_float_emit.ABI_PopRegisters(fprs_to_push, ARM64Reg::X30);
    ABI_PopRegisters(gprs_to_push & ~gprs_to_push_early);

    if (memcheck)
    {
      const ARM64Reg temp_fpr = fprs_to_push[0] ? ARM64Reg::INVALID_REG : ARM64Reg::Q0;
      const u64 early_push_count = (gprs_to_push & gprs_to_push_early).Count();
      const u64 early_push_size = Common::AlignUp(early_push_count, 2) * 8;

      WriteConditionalExceptionExit(EXCEPTION_DSI, temp_gpr, temp_fpr, early_push_size);
    }

    if (flags & BackPatchInfo::FLAG_LOAD)
    {
      ARM64Reg src_reg = access_size == 64 ? ARM64Reg::X0 : ARM64Reg::W0;

      if (flags & BackPatchInfo::FLAG_PAIR)
      {
        // Compensate for the Read_ functions swapping the whole read instead of each pair
        const ARM64Reg dst_reg = flags & BackPatchInfo::FLAG_FLOAT ? src_reg : RS;
        SwapPairs(this, dst_reg, src_reg, flags);
        src_reg = dst_reg;
      }

      if (flags & BackPatchInfo::FLAG_FLOAT)
      {
        if (access_size == 64)
          m_float_emit.FMOV(EncodeRegToDouble(RS), src_reg);
        else
          m_float_emit.FMOV(EncodeRegToSingle(RS), src_reg);

        src_reg = RS;
      }

      ByteswapAfterLoad(this, &m_float_emit, RS, src_reg, flags, false, false);
    }

    ABI_PopRegisters(gprs_to_push & gprs_to_push_early);

    if (gather_pipe_done)
      SetJumpTarget(*gather_pipe_done);
  }

  if (in_far_code)
  {
    if (slow_access_fixup)
    {
      FixupBranch done = B();
      SwitchToNearCode();
      SetJumpTarget(done);
    }
    else
    {
      RET(ARM64Reg::X30);
      SwitchToNearCode();
    }
  }
}

bool JitArm64::HandleFastmemFault(SContext* ctx)
{
  const u8* pc = reinterpret_cast<const u8*>(ctx->CTX_PC);
  auto slow_handler_iter = m_fault_to_handler.upper_bound(pc);

  // no fastmem area found
  if (slow_handler_iter == m_fault_to_handler.end())
    return false;

  const u8* fastmem_area_start = slow_handler_iter->second.fast_access_code;
  const u8* fastmem_area_end = slow_handler_iter->first;

  // no overlapping fastmem area found
  if (pc < fastmem_area_start)
    return false;

  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  ARM64XEmitter emitter(const_cast<u8*>(fastmem_area_start), const_cast<u8*>(fastmem_area_end));

  emitter.BL(slow_handler_iter->second.slow_access_code);

  while (emitter.GetCodePtr() < fastmem_area_end)
    emitter.NOP();

  m_fault_to_handler.erase(slow_handler_iter);

  emitter.FlushIcache();

  ctx->CTX_PC = reinterpret_cast<std::uintptr_t>(fastmem_area_start);
  return true;
}
