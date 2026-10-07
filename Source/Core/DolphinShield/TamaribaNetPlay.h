// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Dolphin's own NetPlay inside the libretro core, for Tamariba's online rooms
// (docs/online-play-design-2026-10-06.md in Tamariba). Every player's device runs Dolphin's
// NetPlayClient; the host's also runs NetPlayServer, on 127.0.0.1 only. The others reach it
// through the Tamariba Plaza's loopback relay, which gives each of them a 127.0.0.1 port that
// stands for the host's server: no device ever learns another's address, and there is no
// traversal server, no UPnP and no NetPlay index.
//
// The frontend asks for it between retro_load_game and the start of rendering, with the C
// functions at the end of this file. From then on the game does not boot by itself: Dolphin's
// NetPlay starts it (after syncing settings, saves and codes from the host) through
// NetPlayUI::BootGame, on the frontend's thread inside retro_run. If NetPlay cannot start
// (the host never answers, versions differ), the game boots on its own instead and the
// status says why.
//
// Saves follow Dolphin's NetPlay rules: the host's memory cards and Wii saves are sent to the
// others, who play on temporary copies (their own saves are never touched); only the host's
// are written back.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <libretro.h>

class BootSessionData;

namespace TamaribaNetPlay
{
// Libretro.cpp's side. Everything here runs on the frontend's (host) thread unless it says so.

// Boots `path` with NetPlay's session data (the host's settings, the save redirection).
using BootFunction = std::function<bool(const std::string& path, BootSessionData data)>;
using MessageFunction = std::function<void(const std::string& text)>;

// A new game was loaded: forgets any earlier session.
void Prepare(const std::string& game_path, BootFunction boot, MessageFunction tell);
// The frontend asked for NetPlay: the game waits for NetPlay to start it.
bool Wanted();
// A NetPlay game is running (no resets, states or controller changes while it does).
bool Running();

enum class Before
{
  Wait,     // not yet
  Booted,   // NetPlay started the game (through the boot function)
  Offline,  // NetPlay could not start it: boot the game on its own
};
// Each retro_run until the game has booted.
Before PumpBeforeBoot();
// Each retro_run while it runs: true when NetPlay ended the game (the host stopped it, the
// connection was lost) and it should stop.
bool PumpRunning();
// Before the game stops: no one waits for the others' buttons any more.
void BeforeStop();
// The game is unloading: leaves the session (closes the server on the host).
void Shutdown();
}  // namespace TamaribaNetPlay

// --- The frontend's side (Tamariba's src/emu/libretro_core.cpp looks these up softly) -----------

extern "C" {

enum dolphin_netplay_state
{
  DOLPHIN_NETPLAY_OFF = 0,
  DOLPHIN_NETPLAY_CONNECTING = 1,  // reaching the host's server
  DOLPHIN_NETPLAY_WAITING = 2,     // in the session, waiting for the other players
  DOLPHIN_NETPLAY_SYNCING = 3,     // the host's saves and codes are on their way
  DOLPHIN_NETPLAY_PLAYING = 4,
  DOLPHIN_NETPLAY_ENDED = 5,       // the host ended it, or the connection was lost
  DOLPHIN_NETPLAY_FAILED = 6,      // could not start: the game runs on its own
};

#define DOLPHIN_NETPLAY_STATUS_VERSION 1
struct dolphin_netplay_status
{
  uint32_t version;         // DOLPHIN_NETPLAY_STATUS_VERSION
  int32_t state;            // dolphin_netplay_state
  int32_t players;          // in the session, this one included
  int32_t pad_buffer;       // Dolphin's pad buffer (polls of input in flight); -1 unknown
  int32_t ping_ms;          // the longest round trip to the host among the players; -1 unknown
  uint32_t desyncs;         // times Dolphin saw the games drift apart
  uint32_t game_ms;         // the console's own clock since the game started (it stands still
                            // while the game waits for another player's buttons)
  uint32_t notice_serial;   // grows with every new notice
  char notice[128];         // the latest thing to say ("Sam joined"), UTF-8
};

// Host. Starts Dolphin's NetPlay server on 127.0.0.1 (*port: the port to use, 0 any free one;
// set to the port it listens on) and joins it. `seats`: the room's seats expected to play
// (bit i: seat i; the host is seat 0); the game starts once they are all in (or after a
// while with whoever is). Returns 0, or -1 when the server could not start.
RETRO_API int dolphin_netplay_host(uint16_t* port, uint32_t seats, const char* nickname);
// Guest. Joins the server reachable at address:port (Tamariba's loopback relay, 127.0.0.1),
// as the player in room seat `seat` (it plays that controller). Returns 0, or -1.
RETRO_API int dolphin_netplay_join(const char* address, uint16_t port, const char* nickname,
                                   int seat);
// Host: the seats expected, when the room changes before the game starts.
RETRO_API void dolphin_netplay_expect(uint32_t seats);
// Host: the pad buffer (1..40), or 0 for automatic (from the players' round trips).
RETRO_API void dolphin_netplay_set_buffer(int buffer);
RETRO_API void dolphin_netplay_leave(void);
// Any thread. Fills *status (its version set by the caller); returns 0, or -1 for a version
// this core does not know.
RETRO_API int dolphin_netplay_get_status(struct dolphin_netplay_status* status);
}
