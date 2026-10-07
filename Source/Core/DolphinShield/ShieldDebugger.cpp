#include "DolphinShield/ShieldDebugger.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/GeckoCode.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace
{
std::atomic<bool> s_enabled{false};
std::atomic<bool> s_trace_enabled{false};
std::mutex s_command_mutex;
std::mutex s_trace_mutex;
std::vector<u32> s_trace_pcs;

bool ParseU32(std::string_view text, u32* value)
{
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  int base = 10;
  if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
  {
    text.remove_prefix(2);
    base = 16;
  }
  if (text.empty())
    return false;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), *value, base);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::string Hex(u32 value)
{
  char buffer[11];
  std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
  return buffer;
}

std::vector<std::string> Tokens(std::string_view command)
{
  std::istringstream input{std::string(command)};
  std::vector<std::string> result;
  for (std::string token; input >> token;)
    result.push_back(std::move(token));
  return result;
}

std::string Error(std::string_view message)
{
  return "ERR " + std::string(message);
}

void WriteResponse(char* response, std::size_t size, const std::string& value)
{
  if (!response || size == 0)
    return;
  const std::size_t count = std::min(size - 1, value.size());
  std::memcpy(response, value.data(), count);
  response[count] = '\0';
}

std::string ReadMemory(Core::CPUThreadGuard& guard, u32 address, u32 size)
{
  if (size == 0 || size > 1024 * 1024)
    return Error("invalid size");
  std::string result;
  result.reserve(size * 2);
  for (u32 offset = 0; offset < size; ++offset)
  {
    const auto value = PowerPC::MMU::HostTryReadU8(guard, address + offset);
    if (!value)
      return Error("unmapped address at " + Hex(address + offset));
    char byte[3];
    std::snprintf(byte, sizeof(byte), "%02x", value->value);
    result += byte;
  }
  return result;
}

std::string Registers(const PowerPC::PowerPCState& state)
{
  std::ostringstream output;
  output << "pc=" << Hex(state.pc) << " npc=" << Hex(state.npc);
  for (int index = 0; index < 32; ++index)
    output << " r" << index << '=' << Hex(state.gpr[index]);
  output << " lr=" << Hex(state.spr[SPR_LR]) << " ctr=" << Hex(state.spr[SPR_CTR]);
  output << " cr=" << Hex(state.cr.Get()) << " xer=" << Hex(state.GetXER().Hex);
  return output.str();
}

std::string Execute(std::string_view command)
{
  const auto args = Tokens(command);
  if (args.empty())
    return Error("empty command");
  Core::System& system = Core::System::GetInstance();
  if (!Core::IsRunning(system))
    return Error("Dolphin is not running");
  Core::CPUThreadGuard guard(system);
  auto& debug = system.GetPowerPC().GetDebugInterface();

  if (args[0] == "status")
  {
    return "enabled=1 pc=" + Hex(system.GetPPCState().pc) + " breakpoints=" +
           std::to_string(system.GetPowerPC().GetBreakPoints().GetBreakPoints().size()) +
           " memchecks=" +
           std::to_string(system.GetPowerPC().GetMemChecks().GetMemChecks().size()) + " patches=" +
           std::to_string(debug.GetPatches().size());
  }
  if (args[0] == "registers")
    return Registers(system.GetPPCState());
  if (args[0] == "trace" && args.size() == 2)
  {
    if (args[1] == "on")
    {
      s_trace_enabled = true;
      return "OK";
    }
    if (args[1] == "off")
    {
      s_trace_enabled = false;
      return "OK";
    }
    if (args[1] == "clear")
    {
      std::lock_guard lock(s_trace_mutex);
      s_trace_pcs.clear();
      return "OK";
    }
    if (args[1] == "dump")
    {
      std::lock_guard lock(s_trace_mutex);
      std::ostringstream output;
      for (std::size_t index = 0; index < s_trace_pcs.size(); ++index)
      {
        if (index)
          output << ',';
        output << Hex(s_trace_pcs[index]);
      }
      return output.str();
    }
    return Error("trace expects on, off, clear, or dump");
  }
  if (args[0] == "read" && args.size() == 3)
  {
    u32 address = 0;
    u32 size = 0;
    if (!ParseU32(args[1], &address) || !ParseU32(args[2], &size))
      return Error("invalid read arguments");
    return ReadMemory(guard, address, size);
  }
  if (args[0] == "pointer" && args.size() == 3)
  {
    u32 base = 0;
    u32 offset = 0;
    if (!ParseU32(args[1], &base) || !ParseU32(args[2], &offset))
      return Error("invalid pointer arguments");
    const auto pointer = PowerPC::MMU::HostTryReadU32(guard, base);
    if (!pointer)
      return Error("unmapped pointer address");
    const u32 target = pointer->value + offset;
    const auto value = PowerPC::MMU::HostTryReadU32(guard, target);
    if (!value)
      return Error("unmapped pointer target at " + Hex(target));
    return Hex(target) + '=' + Hex(value->value);
  }
  if (args[0] == "write" && args.size() == 3)
  {
    u32 address = 0;
    u32 value = 0;
    if (!ParseU32(args[1], &address) || !ParseU32(args[2], &value))
      return Error("invalid write arguments");
    if (!PowerPC::MMU::HostTryWriteU32(guard, value, address))
      return Error("unmapped address");
    return "OK";
  }
  if ((args[0] == "break-add" || args[0] == "break-remove") && args.size() == 2)
  {
    u32 address = 0;
    if (!ParseU32(args[1], &address))
      return Error("invalid breakpoint address");
    if (args[0] == "break-add")
      debug.SetBreakpoint(address);
    else
      debug.ClearBreakpoint(address);
    return "OK";
  }
  if (args[0] == "memcheck" && (args.size() == 3 || args.size() == 4))
  {
    u32 address = 0;
    u32 size = 0;
    if (!ParseU32(args[1], &address) || !ParseU32(args[2], &size) || size == 0)
      return Error("invalid memcheck arguments");
    TMemCheck check;
    check.start_address = address;
    check.end_address = address + size - 1;
    check.is_ranged = size > 1;
    check.is_break_on_read = args.size() == 3 || args[3].find('r') != std::string::npos;
    check.is_break_on_write = args.size() == 3 || args[3].find('w') != std::string::npos;
    check.log_on_hit = args.size() == 4 && args[3].find('l') != std::string::npos;
    check.break_on_hit = true;
    system.GetPowerPC().GetMemChecks().Add(std::move(check));
    return "OK";
  }
  if (args[0] == "patch" && args.size() == 3)
  {
    u32 address = 0;
    u32 value = 0;
    if (!ParseU32(args[1], &address) || !ParseU32(args[2], &value))
      return Error("invalid patch arguments");
    debug.SetPatch(guard, address, value);
    return "OK";
  }
  if (args[0] == "patch-bytes" && args.size() == 3)
  {
    u32 address = 0;
    if (!ParseU32(args[1], &address) || args[2].empty() || args[2].size() % 2 != 0)
      return Error("invalid patch-bytes arguments");
    std::vector<u8> bytes;
    bytes.reserve(args[2].size() / 2);
    for (std::size_t index = 0; index < args[2].size(); index += 2)
    {
      u32 value = 0;
      const std::string byte = "0x" + args[2].substr(index, 2);
      if (!ParseU32(byte, &value) || value > 0xff)
        return Error("invalid patch byte");
      bytes.push_back(static_cast<u8>(value));
    }
    debug.SetPatch(guard, address, std::move(bytes));
    return "OK";
  }
  if (args[0] == "patch-clear")
  {
    debug.ClearPatches(guard);
    return "OK";
  }
  if (args[0] == "code" && args.size() >= 3 && (args.size() - 1) % 2 == 0)
  {
    Gecko::GeckoCode code;
    code.name = "ADB code";
    code.enabled = true;
    for (std::size_t index = 1; index < args.size(); index += 2)
    {
      u32 address = 0;
      u32 value = 0;
      if (!ParseU32(args[index], &address) || !ParseU32(args[index + 1], &value))
        return Error("invalid code pair");
      code.codes.push_back({address, value, args[index] + " " + args[index + 1]});
    }
    Gecko::SetAndReturnActiveCodes(std::span<const Gecko::GeckoCode>(&code, 1));
    return "OK";
  }
  if (args[0] == "disassemble" && args.size() == 2)
  {
    u32 address = 0;
    if (!ParseU32(args[1], &address))
      return Error("invalid disassembly address");
    return debug.Disassemble(&guard, address);
  }
  return Error("unknown command");
}
}  // namespace

void ShieldDebuggerSetEnabled(bool enabled)
{
  s_enabled = enabled;
  if (!enabled)
    s_trace_enabled = false;
}

int ShieldDebuggerCommand(const char* command, char* response, std::size_t response_size)
{
  std::lock_guard lock(s_command_mutex);
  if (!s_enabled)
  {
    WriteResponse(response, response_size, "ERR debugger disabled");
    return -1;
  }
  const std::string result = Execute(command ? command : "");
  WriteResponse(response, response_size, result);
  return result.starts_with("ERR ") ? -1 : static_cast<int>(result.size());
}

void ShieldDebuggerSample()
{
  if (!s_enabled || !s_trace_enabled)
    return;
  Core::System& system = Core::System::GetInstance();
  if (!Core::IsRunning(system))
    return;
  Core::CPUThreadGuard guard(system);
  std::lock_guard lock(s_trace_mutex);
  s_trace_pcs.push_back(system.GetPPCState().pc);
  if (s_trace_pcs.size() > 65536)
    s_trace_pcs.erase(s_trace_pcs.begin(), s_trace_pcs.begin() + 1024);
}
