// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE_Misc.h"

#include <cmath>
#include <cstring>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/Swap.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE_Misc
{
// If you just want to kill a function, one of the three following are usually appropriate.
// According to the PPC ABI, the return value is always in r3.
void UnimplementedFunction(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  ppc_state.npc = LR(ppc_state);
}

void HBReload(const Core::CPUThreadGuard& guard)
{
  // There isn't much we can do. Just stop cleanly.
  auto& system = guard.GetSystem();
  system.GetCPU().Break();
  Host_Message(HostMessageID::WMUserStop);
}

void GeckoCodeHandlerICacheFlush(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  auto& jit_interface = system.GetJitInterface();

  // Work around the codehandler not properly invalidating the icache, but
  // only the first few frames.
  // (Project M uses a conditional to only apply patches after something has
  // been read into memory, or such, so we do the first 5 frames.  More
  // robust alternative would be to actually detect memory writes, but that
  // would be even uglier.)
  u32 gch_gameid = PowerPC::MMU::HostRead_U32(guard, Gecko::INSTALLER_BASE_ADDRESS);
  if (gch_gameid - Gecko::MAGIC_GAMEID == 5)
  {
    return;
  }
  else if (gch_gameid - Gecko::MAGIC_GAMEID > 5)
  {
    gch_gameid = Gecko::MAGIC_GAMEID;
  }
  PowerPC::MMU::HostWrite_U32(guard, gch_gameid + 1, Gecko::INSTALLER_BASE_ADDRESS);

  ppc_state.iCache.Reset(jit_interface);
}

// Because Dolphin messes around with the CPU state instead of patching the game binary, we
// need a way to branch into the GCH from an arbitrary PC address. Branching is easy, returning
// back is the hard part. This HLE function acts as a trampoline that restores the original LR, SP,
// and PC before the magic, invisible BL instruction happened.
void GeckoReturnTrampoline(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  // Stack frame is built in GeckoCode.cpp, Gecko::RunCodeHandler.
  const u32 SP = ppc_state.gpr[1];
  ppc_state.gpr[1] = PowerPC::MMU::HostRead_U32(guard, SP + 8);
  ppc_state.npc = PowerPC::MMU::HostRead_U32(guard, SP + 12);
  LR(ppc_state) = PowerPC::MMU::HostRead_U32(guard, SP + 16);
  ppc_state.cr.Set(PowerPC::MMU::HostRead_U32(guard, SP + 20));
  for (int i = 0; i < 14; ++i)
  {
    ppc_state.ps[i].SetBoth(PowerPC::MMU::HostRead_U64(guard, SP + 24 + 2 * i * sizeof(u64)),
                            PowerPC::MMU::HostRead_U64(guard, SP + 24 + (2 * i + 1) * sizeof(u64)));
  }
}

// 64 x 64 -> 128-bit product
static void Multiply64(u64 a, u64 b, u64* high, u64* low)
{
  const u64 a_low = a & 0xFFFFFFFF;
  const u64 a_high = a >> 32;
  const u64 b_low = b & 0xFFFFFFFF;
  const u64 b_high = b >> 32;
  const u64 p0 = a_low * b_low;
  const u64 p1 = a_low * b_high;
  const u64 p2 = a_high * b_low;
  const u64 middle = (p0 >> 32) + (p1 & 0xFFFFFFFF) + (p2 & 0xFFFFFFFF);
  *low = (middle << 32) | (p0 & 0xFFFFFFFF);
  *high = a_high * b_high + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
}

static bool Greater128(u64 a_high, u64 a_low, u64 b_high, u64 b_low)
{
  return a_high > b_high || (a_high == b_high && a_low > b_low);
}

// Square root of a positive normal double, rounded to nearest. Computed with integers, so the
// result doesn't depend on the host's rounding mode.
static u64 SqrtRoundedToNearest(u64 bits)
{
  // x = mantissa * 2^exponent, with an even exponent
  u64 mantissa = (bits & 0x000FFFFFFFFFFFFFULL) | 0x0010000000000000ULL;
  int exponent = static_cast<int>(bits >> 52) - 1075;
  if (exponent & 1)
  {
    mantissa <<= 1;
    exponent -= 1;
  }

  // root = floor(sqrt(n)), n = mantissa << 52 (2^104 <= n < 2^106, so 2^52 <= root < 2^53)
  const u64 n_high = mantissa >> 12;
  const u64 n_low = mantissa << 52;
  u64 root = static_cast<u64>(std::sqrt(static_cast<double>(mantissa)) * 67108864.0);
  u64 square_high, square_low;
  while (true)
  {
    Multiply64(root, root, &square_high, &square_low);
    if (!Greater128(square_high, square_low, n_high, n_low))
      break;
    --root;
  }
  while (true)
  {
    u64 next_high, next_low;
    Multiply64(root + 1, root + 1, &next_high, &next_low);
    if (Greater128(next_high, next_low, n_high, n_low))
      break;
    ++root;
    square_low = next_low;
  }

  // sqrt(n) > root + 1/2  <=>  n - root^2 > root (a tie is impossible)
  if (n_low - square_low > root)
    ++root;
  u64 result_exponent = static_cast<u64>((exponent - 52) / 2 + 52 + 1023);
  if (root == (1ULL << 53))
  {
    root >>= 1;
    ++result_exponent;
  }
  return (result_exponent << 52) | (root & 0x000FFFFFFFFFFFFFULL);
}

// A host pointer for guest memory that the default BAT setup maps to MEM1 or MEM2, if the access
// is entirely inside it.
static u8* GetRAMPointer(Core::System& system, u32 address, u32 size)
{
  if ((address >> 28) != 0x8 && (address >> 28) != 0x9)
    return nullptr;
  auto& memory = system.GetMemory();
  const u32 physical = address & 0x3FFFFFFF;
  if (physical < memory.GetRamSizeReal() && size <= memory.GetRamSizeReal() - physical)
    return memory.GetRAM() + physical;
  const u32 exram_offset = physical & 0x0FFFFFFF;
  if (memory.GetEXRAM() && (physical >> 28) == 0x1 && exram_offset < memory.GetExRamSizeReal() &&
      size <= memory.GetExRamSizeReal() - exram_offset)
  {
    return memory.GetEXRAM() + exram_offset;
  }
  return nullptr;
}

// sqrt of the Metrowerks standard library (HLE.cpp has its code). It loops once per result bit,
// which made it one of the hottest functions in games like Pokemon Battle Revolution. This gives
// the same result for positive normal inputs, which never set errno, and leaves the same stack
// frame behind (the volatile registers it would have clobbered are not reproduced). Other inputs
// run the original code, which sets errno.
void MSLSqrt(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  const u32 function = ppc_state.pc;
  const u32 sp = ppc_state.gpr[1];
  const u64 x = ppc_state.ps[1].PS0AsU64();
  const u32 exponent = static_cast<u32>(x >> 52) & 0x7FF;

  // The rounding step compares a constant (1.0, at an address that depends on the game) with
  // itself; if it were a NaN, the original would truncate instead of rounding.
  u8* const stack = ppc_state.msr.DR ? GetRAMPointer(system, sp - 0x20, 0x18) : nullptr;
  const u8* const load_constant =
      ppc_state.msr.DR ? GetRAMPointer(system, function + 110 * 4, 4) : nullptr;
  const u8* constant = nullptr;
  if (load_constant)
  {
    const s16 offset = static_cast<s16>(Common::swap32(load_constant) & 0xFFFF);
    constant = GetRAMPointer(system, ppc_state.gpr[2] + offset, 8);
  }
  const u64 constant_bits = constant ? Common::swap64(constant) : 0;
  const bool constant_is_nan = (constant_bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL &&
                               (constant_bits & 0x000FFFFFFFFFFFFFULL) != 0;

  if ((x >> 63) != 0 || exponent == 0 || exponent == 0x7FF || !stack || !constant ||
      constant_is_nan)
  {
    // Run the original from its second instruction, as if the first, stwu r1, -0x20(r1), ran.
    PowerPC::MMU::HostWrite_U32(guard, sp, sp - 0x20);
    ppc_state.gpr[1] = sp - 0x20;
    ppc_state.npc = function + 4;
    return;
  }

  const u64 result = SqrtRoundedToNearest(x);
  // The stack frame: back chain, the argument, the result
  const u32 back_chain = Common::swap32(sp);
  const u64 argument = Common::swap64(x);
  const u64 stored_result = Common::swap64(result);
  std::memcpy(stack, &back_chain, sizeof(back_chain));
  std::memcpy(stack + 8, &argument, sizeof(argument));
  std::memcpy(stack + 0x10, &stored_result, sizeof(stored_result));
  ppc_state.ps[1].SetPS0(result);
  ppc_state.npc = LR(ppc_state);
}
}  // namespace HLE_Misc
