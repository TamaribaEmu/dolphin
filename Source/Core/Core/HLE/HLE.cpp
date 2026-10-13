// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE.h"

#include <algorithm>
#include <array>
#include <map>
#include <span>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"

#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HLE/HLE_Misc.h"
#include "Core/HLE/HLE_OS.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/ES/ES.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace HLE
{
// Map addresses to the HLE hook index
static std::map<u32, u32> s_hooked_addresses;

// clang-format off
constexpr std::array<Hook, 24> os_patches{{
    // Placeholder, os_patches[0] is the "non-existent function" index
    {"FAKE_TO_SKIP_0",               HLE_Misc::UnimplementedFunction,       HookType::Replace, HookFlag::Generic},

    // Name doesn't matter, installed in CBoot::BootUp()
    {"HBReload",                     HLE_Misc::HBReload,                    HookType::Replace, HookFlag::Fixed},

    // Debug/OS Support
    {"OSPanic",                      HLE_OS::HLE_OSPanic,                   HookType::Replace, HookFlag::Debug},

    // This needs to be put before vprintf (because vprintf is called indirectly by this)
    {"JUTWarningConsole_f",          HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},

    {"OSReport",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DEBUGPrint",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"WUD_DEBUGPrint",               HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"__DSP_debug_printf",           HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vprintf",                      HLE_OS::HLE_GeneralDebugVPrint,        HookType::Start,   HookFlag::Debug},
    {"printf",                       HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"vdprintf",                     HLE_OS::HLE_LogVDPrint,                HookType::Start,   HookFlag::Debug},
    {"dprintf",                      HLE_OS::HLE_LogDPrint,                 HookType::Start,   HookFlag::Debug},
    {"vfprintf",                     HLE_OS::HLE_LogVFPrint,                HookType::Start,   HookFlag::Debug},
    {"fprintf",                      HLE_OS::HLE_LogFPrint,                 HookType::Start,   HookFlag::Debug},
    {"nlPrintf",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"DWC_Printf",                   HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"RANK_Printf",                  HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug},
    {"puts",                         HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // gcc-optimized printf?
    {"___blank",                     HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Debug}, // used for early init things (normally)
    {"__write_console",              HLE_OS::HLE_write_console,             HookType::Start,   HookFlag::Debug}, // used by sysmenu (+more?)

    {"GeckoCodehandler",             HLE_Misc::GeckoCodeHandlerICacheFlush, HookType::Start,   HookFlag::Fixed},
    {"GeckoHandlerReturnTrampoline", HLE_Misc::GeckoReturnTrampoline,       HookType::Replace, HookFlag::Fixed},
    {"AppLoaderReport",              HLE_OS::HLE_GeneralDebugPrint,         HookType::Start,   HookFlag::Fixed}, // apploader needs OSReport-like function

    // Found by their code (code_hooks below), not by a symbol
    {"MSL_sqrt",                     HLE_Misc::MSLSqrt,                     HookType::Replace, HookFlag::Generic},
}};
// clang-format on
constexpr u32 MSL_SQRT_HOOK_INDEX = 23;
static_assert(os_patches[MSL_SQRT_HOOK_INDEX].function == HLE_Misc::MSLSqrt);

// Library functions recognized by their code instead of by a symbol, so that retail discs, which
// have no symbol map, get them too. Only the JIT uses these: it checks the code whenever it
// compiles the entry point, and code that changes later is compiled (and checked) again.
//
// They are only used with a lowered CPU clock. They make the game's work take fewer emulated
// cycles, which only helps when the game is short of cycles: Pokemon Battle Revolution, for one,
// runs a background thread that busy-loops through all the time it has left, so at the full
// clock the cycles saved just go to that loop (which costs more host time per cycle). With a
// lowered clock, they are what keeps the game at its frame rate.
struct CodeHook
{
  u32 hook_index;
  std::span<const u32> code;
  // Instructions whose 16-bit immediate differs between games (data addresses and offsets)
  std::span<const u32> any_immediate;
};

// double sqrt(double) of the Metrowerks standard library (MSL), as linked into many GameCube and
// Wii games: fdlibm's bit-by-bit square root, which sets errno for invalid inputs.
constexpr std::array<u32, 146> MSL_SQRT_CODE{
    0x9421ffe0, 0xd8210008, 0x80c10008, 0x8001000c, 0x54c30056, 0x3c638010, 0x28030000,
    0x40820018, 0xfc010072, 0x38000021, 0x900d0000, 0xfc21002a, 0x48000210, 0x2c860000,
    0x4185002c, 0x54c3007e, 0x7c031b79, 0x40820008, 0x480001f8, 0x40840018, 0x38000021,
    0x3c600000, 0x900d0000, 0xc0230000, 0x480001e0, 0x7cc3a671, 0x40820050, 0x48000014,
    0x5404aafe, 0x5400a814, 0x7cc62378, 0x3863ffeb, 0x2c060000, 0x4182ffec, 0x38e00000,
    0x4800000c, 0x54c6083c, 0x38e70001, 0x54c402d7, 0x4182fff4, 0x20870020, 0x38a7ffff,
    0x7c042430, 0x7c003830, 0x7c651850, 0x7cc62378, 0x3883fc01, 0x54c5033e, 0x548407ff,
    0x64a60010, 0x41820014, 0x54050ffe, 0x7c863214, 0x7cc52214, 0x7c000214, 0x54050ffe,
    0x7c863214, 0x7cc52214, 0x7c000214, 0x39200000, 0x39600000, 0x39400000, 0x39800000,
    0x3ce00020, 0x48000030, 0x7c8b3a14, 0x7c043000, 0x41810010, 0x7d643a14, 0x7cc43050,
    0x7d8c3a14, 0x54050ffe, 0x7c863214, 0x7cc52214, 0x7c000214, 0x54e7f87e, 0x2c070000,
    0x4082ffd0, 0x3ce08000, 0x4800006c, 0x7c0b3000, 0x7d655b78, 0x7d093a14, 0x41800010,
    0x40820044, 0x7c080040, 0x4181003c, 0x55040000, 0x7d283a14, 0x3c848000, 0x28040000,
    0x40820010, 0x55240001, 0x40820008, 0x396b0001, 0x7c004040, 0x7cc53050, 0x40800008,
    0x38c6ffff, 0x7c080050, 0x7d4a3a14, 0x54050ffe, 0x7c863214, 0x7cc52214, 0x7c000214,
    0x54e7f87e, 0x2c070000, 0x4082ff94, 0x7cc00379, 0x41820058, 0xc8020000, 0xfc000040,
    0xd8010010, 0x4c411382, 0x40820044, 0x3c0a0001, 0xd8010010, 0x2800ffff, 0x40820010,
    0x39400000, 0x398c0001, 0x48000028, 0xfc000040, 0x40810018, 0x2800fffe, 0x40820008,
    0x398c0001, 0x394a0002, 0x4800000c, 0x554007fe, 0x7d4a0214, 0x558007fe, 0x7d840e70,
    0x2c000001, 0x5545f87e, 0x3c843fe0, 0x40820008, 0x64a58000, 0x3803fc01, 0x90a10014,
    0x54009816, 0x7c840214, 0x90810010, 0xc8210010, 0x38210020, 0x4e800020,
};
// errno (twice), the address of the NaN it returns for negative inputs, and the constant 1.0
constexpr std::array<u32, 5> MSL_SQRT_ANY_IMMEDIATE{10, 21, 22, 23, 110};

constexpr std::array<CodeHook, 1> code_hooks{{
    {MSL_SQRT_HOOK_INDEX, MSL_SQRT_CODE, MSL_SQRT_ANY_IMMEDIATE},
}};

static bool CodeMatches(const Core::CPUThreadGuard& guard, u32 address, const CodeHook& hook)
{
  for (u32 i = 0; i < hook.code.size(); ++i)
  {
    const auto word = PowerPC::MMU::HostTryReadInstruction(guard, address + i * 4);
    if (!word)
      return false;
    const bool any_immediate = std::ranges::find(hook.any_immediate, i) != hook.any_immediate.end();
    const u32 mask = any_immediate ? 0xFFFF0000 : 0xFFFFFFFF;
    if ((word->value & mask) != hook.code[i])
      return false;
  }
  return true;
}

static u32 GetHookByCode(u32 address)
{
  // The code is read through the CPU thread's view of memory (the JIT compiles there).
  if (!Core::IsCPUThread())
    return 0;
  if (!Config::Get(Config::MAIN_OVERCLOCK_ENABLE) || Config::Get(Config::MAIN_OVERCLOCK) >= 1.0f)
    return 0;
  Core::System& system = Core::System::GetInstance();
  const Core::CPUThreadGuard guard(system);
  const auto first = PowerPC::MMU::HostTryReadInstruction(guard, address);
  if (!first)
    return 0;
  for (const CodeHook& hook : code_hooks)
  {
    if (first->value == hook.code[0] && CodeMatches(guard, address, hook))
      return hook.hook_index;
  }
  return 0;
}

void Patch(Core::System& system, u32 addr, std::string_view func_name)
{
  auto& ppc_state = system.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  for (u32 i = 1; i < os_patches.size(); ++i)
  {
    if (os_patches[i].name == func_name)
    {
      s_hooked_addresses[addr] = i;
      ppc_state.iCache.Invalidate(memory, jit_interface, addr);
      return;
    }
  }
}

void PatchFixedFunctions(Core::System& system)
{
  // MIOS puts patch data in low MEM1 (0x1800-0x3000) for its own use.
  // Overwriting data in this range can cause the IPL to crash when launching games
  // that get patched by MIOS. See https://bugs.dolphin-emu.org/issues/11952 for more info.
  // Not applying the Gecko HLE patches means that Gecko codes will not work under MIOS,
  // but this is better than the alternative of having specific games crash.
  if (system.IsMIOS())
    return;

  // HLE jump to loader (homebrew).  Disabled when Gecko is active as it interferes with the code
  // handler
  if (!Config::AreCheatsEnabled())
  {
    Patch(system, 0x80001800, "HBReload");
    auto& memory = system.GetMemory();
    memory.CopyToEmu(0x00001804, "STUBHAXX", 8);
  }

  // Not part of the binary itself, but either we or Gecko OS might insert
  // this, and it doesn't clear the icache properly.
  Patch(system, Gecko::ENTRY_POINT, "GeckoCodehandler");
  // This has to always be installed even if cheats are not enabled because of the possiblity of
  // loading a savestate where PC is inside the code handler while cheats are disabled.
  Patch(system, Gecko::HLE_TRAMPOLINE_ADDRESS, "GeckoHandlerReturnTrampoline");
}

void PatchFunctions(Core::System& system)
{
  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();
  auto& ppc_symbol_db = power_pc.GetSymbolDB();

  // Remove all hooks that aren't fixed address hooks
  for (auto i = s_hooked_addresses.begin(); i != s_hooked_addresses.end();)
  {
    if (os_patches[i->second].flags != HookFlag::Fixed)
    {
      ppc_state.iCache.Invalidate(memory, jit_interface, i->first);
      i = s_hooked_addresses.erase(i);
    }
    else
    {
      ++i;
    }
  }

  for (u32 i = 1; i < os_patches.size(); ++i)
  {
    // Fixed hooks don't map to symbols
    if (os_patches[i].flags == HookFlag::Fixed)
      continue;

    for (const auto& symbol : ppc_symbol_db.GetSymbolsFromName(os_patches[i].name))
    {
      for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
      {
        s_hooked_addresses[addr] = i;
        ppc_state.iCache.Invalidate(memory, jit_interface, addr);
      }
      INFO_LOG_FMT(OSHLE, "Patching {} {:08x}", os_patches[i].name, symbol->address);
    }
  }
}

void Clear()
{
  s_hooked_addresses.clear();
}

void Reload(Core::System& system)
{
  Clear();
  PatchFixedFunctions(system);
  PatchFunctions(system);
}

void Execute(const Core::CPUThreadGuard& guard, u32 current_pc, u32 hook_index)
{
  hook_index &= 0xFFFFF;
  if (hook_index > 0 && hook_index < os_patches.size())
  {
    os_patches[hook_index].function(guard);
  }
  else
  {
    PanicAlertFmt("HLE system tried to call an undefined HLE function {}.", hook_index);
  }
}

void ExecuteFromJIT(u32 current_pc, u32 hook_index, Core::System& system)
{
  ASSERT(Core::IsCPUThread());
  Core::CPUThreadGuard guard(system);
  // As the interpreters do: the hook sees the address it was called at
  system.GetPPCState().pc = current_pc;
  Execute(guard, current_pc, hook_index);
}

u32 GetHookByAddress(u32 address)
{
  auto iter = s_hooked_addresses.find(address);
  return (iter != s_hooked_addresses.end()) ? iter->second : 0;
}

u32 GetHookByFunctionAddress(PPCSymbolDB& ppc_symbol_db, u32 address)
{
  const u32 index = GetHookByAddress(address);
  // Fixed hooks use a fixed address and don't patch the whole function
  if (index == 0 || os_patches[index].flags == HookFlag::Fixed)
    return index;

  const Common::Symbol* const symbol = ppc_symbol_db.GetSymbolFromAddr(address);
  return (symbol && symbol->address == address) ? index : 0;
}

HookType GetHookTypeByIndex(u32 index)
{
  return os_patches[index].type;
}

HookFlag GetHookFlagsByIndex(u32 index)
{
  return os_patches[index].flags;
}

TryReplaceFunctionResult TryReplaceFunction(PPCSymbolDB& ppc_symbol_db, u32 address,
                                            PowerPC::CoreMode mode)
{
  u32 hook_index = GetHookByFunctionAddress(ppc_symbol_db, address);
  if (hook_index == 0 && mode == PowerPC::CoreMode::JIT)
    hook_index = GetHookByCode(address);
  if (hook_index == 0)
    return {};

  const HookType type = GetHookTypeByIndex(hook_index);
  if (type != HookType::Start && type != HookType::Replace)
    return {};

  const HookFlag flags = GetHookFlagsByIndex(hook_index);
  if (!IsEnabled(flags, mode))
    return {};

  return {type, hook_index};
}

bool IsEnabled(HookFlag flag, PowerPC::CoreMode mode)
{
  return flag != HLE::HookFlag::Debug || Config::IsDebuggingEnabled() ||
         mode == PowerPC::CoreMode::Interpreter;
}

u32 UnPatch(Core::System& system, std::string_view patch_name)
{
  const auto patch = std::find_if(std::begin(os_patches), std::end(os_patches),
                                  [&](const Hook& p) { return patch_name == p.name; });
  if (patch == std::end(os_patches))
    return 0;

  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();

  if (patch->flags == HookFlag::Fixed)
  {
    const u32 patch_idx = static_cast<u32>(std::distance(os_patches.begin(), patch));
    u32 addr = 0;
    // Reverse search by OSPatch key instead of address
    for (auto i = s_hooked_addresses.begin(); i != s_hooked_addresses.end();)
    {
      if (i->second == patch_idx)
      {
        addr = i->first;
        ppc_state.iCache.Invalidate(memory, jit_interface, i->first);
        i = s_hooked_addresses.erase(i);
      }
      else
      {
        ++i;
      }
    }
    return addr;
  }

  const auto symbols = power_pc.GetSymbolDB().GetSymbolsFromName(patch_name);
  if (!symbols.empty())
  {
    const Common::Symbol* const symbol = symbols.front();
    for (u32 addr = symbol->address; addr < symbol->address + symbol->size; addr += 4)
    {
      s_hooked_addresses.erase(addr);
      ppc_state.iCache.Invalidate(memory, jit_interface, addr);
    }
    return symbol->address;
  }

  return 0;
}

u32 UnpatchRange(Core::System& system, u32 start_addr, u32 end_addr)
{
  auto& ppc_state = system.GetPPCState();
  auto& memory = system.GetMemory();
  auto& jit_interface = system.GetJitInterface();

  u32 count = 0;

  auto i = s_hooked_addresses.lower_bound(start_addr);
  while (i != s_hooked_addresses.end() && i->first < end_addr)
  {
    INFO_LOG_FMT(OSHLE, "Unpatch HLE hooks [{:08x};{:08x}): {} at {:08x}", start_addr, end_addr,
                 os_patches[i->second].name, i->first);
    ppc_state.iCache.Invalidate(memory, jit_interface, i->first);
    i = s_hooked_addresses.erase(i);
    count += 1;
  }

  return count;
}
}  // namespace HLE