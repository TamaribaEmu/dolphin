// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/TamaribaWFC.h"

#include <atomic>
#include <sstream>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Core/GeckoCodeConfig.h"

namespace TamaribaWFC
{
namespace
{
std::atomic<Service> s_service{Service::Off};
}

void SetService(Service service)
{
  s_service = service;
}

Service GetService()
{
  return s_service;
}

Service ParseService(const std::string& name)
{
  if (name == "wiilink")
    return Service::WiiLink;
  if (name == "wiimmfi")
    return Service::Wiimmfi;
  return Service::Off;
}

std::optional<Gecko::GeckoCode> PatchFromText(const std::string& text, const std::string& game_id,
                                              u16 revision)
{
  if (game_id.size() < 4)
    return std::nullopt;
  // A disc: "RMCE" + "D" + its revision; a channel: + "N" + its version.
  const std::string disc = fmt::format("[{}D{:02}]", game_id.substr(0, 4), revision);
  const std::string channel = fmt::format("[{}N{:04}]", game_id.substr(0, 4), revision);
  std::istringstream in(text);
  std::string line;
  bool inside = false;
  Gecko::GeckoCode code;
  code.name = "WiiLink WFC";
  code.creator = "WiiLink";
  code.enabled = true;
  code.default_enabled = true;
  while (std::getline(in, line))
  {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    if (line[0] == '[')
    {
      if (inside)
        break;
      inside = line == disc || line == channel;
      continue;
    }
    if (!inside)
      continue;
    const std::optional<Gecko::GeckoCode::Code> parsed = Gecko::DeserializeLine(line);
    if (!parsed)
      return std::nullopt;  // a damaged file: no half a patch
    code.codes.push_back(*parsed);
  }
  if (code.codes.empty())
    return std::nullopt;
  return code;
}

std::optional<Gecko::GeckoCode> PatchFor(const std::string& game_id, u16 revision)
{
  if (GetService() != Service::WiiLink)
    return std::nullopt;
  std::string text;
  if (!File::ReadFileToString(File::GetSysDirectory() + "TamaribaWFC/WiiLink.txt", text))
    return std::nullopt;
  auto code = PatchFromText(text, game_id, revision);
  INFO_LOG_FMT(CORE, "Tamariba: {} WiiLink WFC patch for {} (revision {})",
               code ? "applying the" : "no", game_id, revision);
  return code;
}
}  // namespace TamaribaWFC
