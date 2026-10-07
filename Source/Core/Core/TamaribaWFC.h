// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tamariba: Nintendo Wi-Fi Connection replacements for Wii games (docs/online-play-design-
// 2026-10-06.md in Tamariba, "The Plaza internet exit"). The player picks a service in
// Tamariba's settings; the console's traffic goes through the Plaza's exit
// (IOS/Network/TamaribaNet.h), whose name lookups follow that choice.
//
//   WiiLink WFC  The game gets WiiLink's own first-stage patch, the Gecko code WiiLink publishes
//                for it (Sys/TamaribaWFC/WiiLink.txt, one section per title): it points the
//                game's login at WiiLink and loads WiiLink's signed payload from there.
//   Wiimmfi      Nothing here: the Plaza answers Nintendo's names with Wiimmfi's servers, and
//                games patched for Wiimmfi (its patcher's disc images) name it themselves.
//
// Both services refuse Dolphin's shared default console identity: playing online needs the
// player's own Wii NAND (Tools › Manage NAND in Dolphin).

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/GeckoCode.h"

namespace TamaribaWFC
{
enum class Service
{
  Off,
  WiiLink,
  Wiimmfi,
};

void SetService(Service service);
Service GetService();
// "off", "wiilink", "wiimmfi" (Tamariba's setting); anything else is Off.
Service ParseService(const std::string& name);

// The running game's WiiLink patch (game ID and revision as SConfig has them), when WiiLink is
// the chosen service and it has one for this game.
std::optional<Gecko::GeckoCode> PatchFor(const std::string& game_id, u16 revision);
// The same from a WiiLink.txt's text (tests).
std::optional<Gecko::GeckoCode> PatchFromText(const std::string& text, const std::string& game_id,
                                              u16 revision);
}  // namespace TamaribaWFC
