// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinShield/TamaribaNetPlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayServer.h"
#include "Core/System.h"
#include "UICommon/GameFile.h"

namespace TamaribaNetPlay
{
namespace
{
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// How long a guest keeps trying to reach the host's server, and how long the host waits for
// the room's players before starting with whoever is in.
constexpr auto kJoinPatience = 60s;
constexpr auto kHostPatience = 45s;
// Once everyone is in: a moment for the first pings, so the pad buffer fits the round trips.
constexpr auto kPingGrace = 3s;
constexpr int kDefaultBuffer = 5;  // Dolphin's own default, until a round trip is known

enum class Role
{
  None,
  Host,
  Guest
};

// --- What the frontend reads (any thread) -----------------------------------------------------

std::mutex s_status_mutex;
dolphin_netplay_status s_status{};

void SetState(dolphin_netplay_state state)
{
  std::lock_guard lock(s_status_mutex);
  s_status.state = state;
}

dolphin_netplay_state GetState()
{
  std::lock_guard lock(s_status_mutex);
  return static_cast<dolphin_netplay_state>(s_status.state);
}

void Notice(const std::string& text)
{
  INFO_LOG_FMT(NETPLAY, "Tamariba: {}", text);
  std::lock_guard lock(s_status_mutex);
  std::snprintf(s_status.notice, sizeof(s_status.notice), "%s", text.c_str());
  ++s_status.notice_serial;
}

// --- The session (host thread, except where noted) -----------------------------------------------

std::string s_game_path;
BootFunction s_boot;
MessageFunction s_tell;
std::shared_ptr<const UICommon::GameFile> s_game;  // read by NetPlay's threads (set before them)
std::atomic<Role> s_role{Role::None};
std::string s_nickname;  // as NetPlay sees it: "<seat>:<name>"
std::unique_ptr<NetPlay::NetPlayServer> s_server;
std::unique_ptr<NetPlay::NetPlayClient> s_client;
std::atomic<bool> s_start_pending{false};  // host: the start was asked for
Clock::time_point s_since;     // when the session began (host: or when all were in)
std::optional<Clock::time_point> s_all_in_since;
std::atomic<uint32_t> s_expected{1};  // host: the seats to wait for
std::atomic<int> s_buffer_override{0};
int s_buffer = 0;  // the buffer the host set last
Clock::time_point s_buffer_checked;

// The connecting thread (a NetPlayClient's constructor waits for the server's answer).
std::thread s_connector;
std::atomic<bool> s_cancel{false};
std::mutex s_handover_mutex;
std::unique_ptr<NetPlay::NetPlayClient> s_connected;  // handed over by the connecting thread
bool s_connect_failed = false;

// Set by NetPlay's threads.
std::atomic<bool> s_start_requested{false};
std::atomic<bool> s_stop_requested{false};
std::atomic<bool> s_connection_error_fatal{false};
std::atomic<bool> s_booted{false};

// "2:Sam" -> seat 2 and "Sam"; seat -1 for a name without one.
std::pair<int, std::string> SplitName(const std::string& name)
{
  if (name.size() >= 2 && name[0] >= '0' && name[0] <= '9' && name[1] == ':')
    return {name[0] - '0', name.substr(2)};
  return {-1, name};
}

// At most NetPlay's 30 characters (code points), cut between characters.
std::string Shorten(const std::string& text)
{
  std::string out;
  u32 count = 0;
  for (char c : text)
  {
    const bool continuation = (static_cast<unsigned char>(c) & 0xC0) == 0x80;
    if (!continuation && ++count > NetPlay::MAX_NAME_LENGTH)
      break;
    out += c;
  }
  return out;
}

// The pad buffer for the longest round trip to the host: enough polls (about 8 ms each, as
// most games poll twice a frame) for a player's buttons to reach every other device, and two
// for jitter. With two players that takes half a round trip (one device is the host); with
// more, a guest's buttons go to the host and on to another guest: a whole one.
int BufferFor(u32 ping_ms, size_t players)
{
  if (ping_ms == 0)
    return kDefaultBuffer;
  const double one_way = players <= 2 ? ping_ms / 2.0 : ping_ms;
  return std::clamp(static_cast<int>(std::ceil(one_way / 8.0)) + 2, 2, 40);
}

class HeadlessUI final : public NetPlay::NetPlayUI
{
public:
  void BootGame(const std::string& filename,
                std::unique_ptr<BootSessionData> boot_session_data) override
  {
    // Called from NetPlayClient::StartGame, which PumpBeforeBoot calls on the host thread.
    if (!s_boot || !boot_session_data)
      return;
    if (s_boot(filename, std::move(*boot_session_data)))
    {
      s_booted = true;
      SetState(DOLPHIN_NETPLAY_PLAYING);
    }
  }
  void StopGame() override
  {
    if (s_booted)
      s_stop_requested = true;
  }
  bool IsHosting() const override { return s_role == Role::Host; }

  void Update() override {}
  void AppendChat(const std::string& msg) override { INFO_LOG_FMT(NETPLAY, "Tamariba: {}", msg); }

  void OnMsgChangeGame(const NetPlay::SyncIdentifier&, const std::string& netplay_name) override
  {
    INFO_LOG_FMT(NETPLAY, "Tamariba: the game is {}", netplay_name);
  }
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig&) override {}
  void OnMsgStartGame() override { s_start_requested = true; }
  void OnMsgStopGame() override {}
  void OnMsgPowerButton() override {}
  void OnPlayerConnect(const std::string& player) override
  {
    Notice(SplitName(player).second + " joined");
  }
  void OnPlayerDisconnect(const std::string& player) override
  {
    Notice(SplitName(player).second + " left");
  }
  void OnPadBufferChanged(u32 buffer) override
  {
    std::lock_guard lock(s_status_mutex);
    s_status.pad_buffer = static_cast<int32_t>(buffer);
  }
  void OnHostInputAuthorityChanged(bool) override {}
  void OnDesync(u32 frame, const std::string& player) override
  {
    {
      std::lock_guard lock(s_status_mutex);
      ++s_status.desyncs;
    }
    Notice(fmt::format("{}'s game drifted apart (frame {})", SplitName(player).second, frame));
  }
  void OnConnectionLost() override
  {
    Notice("Lost the connection to the host");
    if (s_booted)
    {
      SetState(DOLPHIN_NETPLAY_ENDED);
      s_stop_requested = true;
    }
    else
    {
      SetState(DOLPHIN_NETPLAY_FAILED);  // PumpBeforeBoot starts the game on its own
    }
  }
  void OnConnectionError(const std::string& message) override
  {
    // "Could not communicate with host" is worth trying again (the host's server may not be
    // up yet); the others (versions differ, the game already runs, full) are not.
    if (message.find("communicate") == std::string::npos)
      s_connection_error_fatal = true;
    INFO_LOG_FMT(NETPLAY, "Tamariba: {}", message);
    std::lock_guard lock(s_status_mutex);
    std::snprintf(s_status.notice, sizeof(s_status.notice), "%s", message.c_str());
  }
  void OnTraversalError(Common::TraversalClient::FailureReason) override {}
  void OnTraversalStateChanged(Common::TraversalClient::State) override {}
  void OnGameStartAborted() override
  {
    s_start_pending = false;
    SetState(DOLPHIN_NETPLAY_WAITING);
    Notice("The start was called off");
  }
  void OnGolferChanged(bool, const std::string&) override {}
  void OnTtlDetermined(u8) override {}

  bool IsRecording() override { return false; }
  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
               NetPlay::SyncIdentifierComparison* found) override
  {
    // The only game there is: the one the frontend loaded.
    NetPlay::SyncIdentifierComparison result = NetPlay::SyncIdentifierComparison::DifferentGame;
    std::shared_ptr<const UICommon::GameFile> game = s_game;
    if (game)
      result = game->CompareSyncIdentifier(sync_identifier);
    if (found)
      *found = result;
    return result == NetPlay::SyncIdentifierComparison::SameGame ? game : nullptr;
  }
  std::string FindGBARomPath(const std::array<u8, 20>&, std::string_view, int) override
  {
    return {};
  }
  void ShowGameDigestDialog(const std::string&) override {}
  void SetGameDigestProgress(int, int) override {}
  void SetGameDigestResult(int, const std::string&) override {}
  void AbortGameDigest() override {}

  void OnIndexAdded(bool, std::string) override {}
  void OnIndexRefreshFailed(std::string) override {}

  void ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                 const std::vector<int>&) override
  {
    INFO_LOG_FMT(NETPLAY, "Tamariba: sending {} ({} bytes)", title, data_size);
    if (!s_booted)
      SetState(DOLPHIN_NETPLAY_SYNCING);
  }
  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}

  void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) override
  {
    // From NetPlayServer::RequestStartGame, which runs on the host thread like s_client's owner.
    if (s_client)
      s_client->SetWiiSyncData(nullptr, std::move(titles), std::move(redirect_folder));
  }
};

HeadlessUI s_ui;

NetPlay::NetTraversalConfig LoopbackOnly()
{
  NetPlay::NetTraversalConfig config;
  config.use_traversal = false;
  config.loopback_only = true;
  return config;
}

// NetPlay's settings for a Tamariba session (the host's are the ones that count).
void ApplySettings()
{
  Config::SetBase(Config::NETPLAY_USE_INDEX, false);
  Config::SetBase(Config::NETPLAY_USE_UPNP, false);
  Config::SetBase(Config::NETPLAY_ENABLE_QOS, false);
  Config::SetBase(Config::NETPLAY_TRAVERSAL_CHOICE, std::string("direct"));
  // The host's saves for everyone, on temporary copies; only the host's are written back.
  Config::SetBase(Config::NETPLAY_SAVEDATA_LOAD, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_WRITE, true);
  Config::SetBase(Config::NETPLAY_SAVEDATA_SYNC_ALL_WII, false);
  Config::SetBase(Config::NETPLAY_SYNC_CODES, true);
  Config::SetBase(Config::NETPLAY_RECORD_INPUTS, false);
  Config::SetBase(Config::NETPLAY_NETWORK_MODE, std::string("fixeddelay"));
}

// Starts the thread that reaches the server (the host's own included) and hands the client over.
void Connect(std::string address, u16 port, Clock::duration patience)
{
  s_cancel = false;
  s_connection_error_fatal = false;
  {
    std::lock_guard lock(s_handover_mutex);
    s_connected.reset();
    s_connect_failed = false;
  }
  s_connector = std::thread([address = std::move(address), port, patience] {
    const Clock::time_point deadline = Clock::now() + patience;
    while (!s_cancel)
    {
      auto client = std::make_unique<NetPlay::NetPlayClient>(address, port, &s_ui, s_nickname,
                                                             LoopbackOnly());
      if (client->IsConnected())
      {
        std::lock_guard lock(s_handover_mutex);
        s_connected = std::move(client);
        return;
      }
      client.reset();
      if (s_connection_error_fatal || Clock::now() >= deadline)
        break;
      for (int i = 0; i < 10 && !s_cancel; ++i)
        std::this_thread::sleep_for(100ms);
    }
    std::lock_guard lock(s_handover_mutex);
    s_connect_failed = true;
  });
}

void StopConnecting()
{
  s_cancel = true;
  if (s_connector.joinable())
    s_connector.join();
  std::lock_guard lock(s_handover_mutex);
  s_connected.reset();
}

// The pad buffer the players' round trips ask for.
int AutomaticBuffer()
{
  if (!s_client)
    return kDefaultBuffer;
  const auto players = s_client->GetPlayers();
  u32 longest = 0;
  for (const NetPlay::Player* player : players)
    longest = std::max(longest, player->ping);
  return BufferFor(longest, players.size());
}

void UpdatePlayers()
{
  if (!s_client)
    return;
  const auto players = s_client->GetPlayers();
  u32 longest = 0;
  for (const NetPlay::Player* player : players)
    longest = std::max(longest, player->ping);
  std::lock_guard lock(s_status_mutex);
  s_status.players = static_cast<int32_t>(players.size());
  s_status.ping_ms = players.size() > 1 ? static_cast<int32_t>(longest) : -1;
}

// Host: once everyone expected is in with the same game (or the wait is over), every seat
// gets its controller (room seat N plays controller N, GameCube pad and Wii Remote) and the
// start is asked for: Dolphin then syncs saves and codes and starts everyone's game.
void MaybeStart()
{
  const auto players = s_client->GetPlayers();
  uint32_t present = 0;
  for (const NetPlay::Player* player : players)
  {
    const int seat = player->IsHost() ? 0 : SplitName(player->name).first;
    if (seat >= 0 && seat < 4 && player->game_status == NetPlay::SyncIdentifierComparison::SameGame)
      present |= 1u << seat;
  }
  const uint32_t expected = s_expected.load() | 1u;
  const Clock::time_point now = Clock::now();
  const bool all_in = (expected & ~present) == 0;
  if (all_in && !s_all_in_since)
    s_all_in_since = now;
  bool pings_known = true;
  for (const NetPlay::Player* player : players)
    pings_known = pings_known && (player->IsHost() || player->ping > 0);
  const bool ready = all_in && (pings_known || now - *s_all_in_since >= kPingGrace);
  if (!ready && now - s_since < kHostPatience)
    return;

  NetPlay::PadMappingArray pads{};
  for (const NetPlay::Player* player : players)
  {
    const int seat = player->IsHost() ? 0 : SplitName(player->name).first;
    const bool usable =
        seat >= 0 && seat < 4 && player->game_status == NetPlay::SyncIdentifierComparison::SameGame;
    if (usable && pads[seat] == 0)
      pads[seat] = player->pid;
    else if (!player->IsHost())
      s_server->KickPlayer(player->pid);  // not the room's game (or no seat): it can't play
  }
  s_server->SetPadMapping(pads);
  s_server->SetWiimoteMapping(pads);
  const int override_buffer = s_buffer_override.load();
  s_buffer = override_buffer > 0 ? override_buffer : AutomaticBuffer();
  s_server->AdjustPadBufferSize(static_cast<unsigned>(s_buffer));
  s_buffer_checked = now;
  INFO_LOG_FMT(NETPLAY, "Tamariba: starting with seats {:#x} of {:#x}, pad buffer {}", present,
               expected, s_buffer);
  s_start_pending = true;
  SetState(DOLPHIN_NETPLAY_SYNCING);
  if (!s_server->RequestStartGame())
  {
    s_start_pending = false;
    SetState(DOLPHIN_NETPLAY_FAILED);
  }
}

// Host, while playing: the pad buffer follows the round trips (unless it was set by hand).
void FollowPings()
{
  const Clock::time_point now = Clock::now();
  if (now - s_buffer_checked < 5s)
    return;
  s_buffer_checked = now;
  if (s_buffer_override > 0)
  {
    if (s_buffer != s_buffer_override)
      s_server->AdjustPadBufferSize(static_cast<unsigned>(s_buffer = s_buffer_override));
    return;
  }
  const int wanted = AutomaticBuffer();
  if (std::abs(wanted - s_buffer) >= 2)
  {
    s_buffer = wanted;
    s_server->AdjustPadBufferSize(static_cast<unsigned>(s_buffer));
  }
}

// Out of the session, keeping its status for the frontend.
void Drop()
{
  StopConnecting();
  s_client.reset();  // stops its thread (and the game's NetPlay side)
  s_server.reset();
  s_role = Role::None;
}

void Reset()
{
  Drop();
  s_start_pending = false;
  s_all_in_since.reset();
  s_start_requested = false;
  s_stop_requested = false;
  s_booted = false;
  s_buffer = 0;
  std::lock_guard lock(s_status_mutex);
  s_status = {};
  s_status.version = DOLPHIN_NETPLAY_STATUS_VERSION;
  s_status.pad_buffer = -1;
  s_status.ping_ms = -1;
}

bool Begin(Role role, const char* nickname, int seat)
{
  if (s_game_path.empty() || s_role != Role::None)
    return false;
  s_game = std::make_shared<UICommon::GameFile>(s_game_path);
  if (!s_game->IsValid())
  {
    s_game.reset();
    return false;
  }
  ApplySettings();
  s_role = role;
  s_nickname = Shorten(fmt::format("{}:{}", seat, nickname && *nickname ? nickname : "Player"));
  s_since = Clock::now();
  SetState(DOLPHIN_NETPLAY_CONNECTING);
  return true;
}
}  // namespace

void Prepare(const std::string& game_path, BootFunction boot, MessageFunction tell)
{
  Reset();
  s_game.reset();
  s_game_path = game_path;
  s_boot = std::move(boot);
  s_tell = std::move(tell);
}

bool Wanted()
{
  return s_role != Role::None;
}

bool Running()
{
  return s_booted && NetPlay::IsNetPlayRunning();
}

Before PumpBeforeBoot()
{
  if (s_role == Role::None)
    return Before::Offline;
  if (!s_client)
  {
    bool failed = false;
    {
      std::lock_guard lock(s_handover_mutex);
      if (s_connected)
      {
        s_client = std::move(s_connected);
        SetState(DOLPHIN_NETPLAY_WAITING);
      }
      failed = !s_client && s_connect_failed;
    }
    if (failed)
    {
      const bool host = s_role == Role::Host;
      Drop();
      SetState(DOLPHIN_NETPLAY_FAILED);
      if (s_tell)
        s_tell(host ? "Couldn't start the online game: playing on your own" :
                      "Couldn't reach the host's game: playing on your own");
      return Before::Offline;
    }
  }
  if (!s_client)
    return Before::Wait;
  if (s_connector.joinable())
    s_connector.join();  // it handed the client over and ended
  UpdatePlayers();
  if (s_start_requested.exchange(false))
  {
    // As Dolphin's own dialog does on its GUI thread: NetPlay boots the game through
    // HeadlessUI::BootGame, here, with the host's settings and the synced saves.
    s_client->StartGame(s_game_path);
    if (s_booted)
      return Before::Booted;
    SetState(DOLPHIN_NETPLAY_FAILED);
    return Before::Wait;  // the boot failed; the frontend shows the error
  }
  if (!s_client->IsConnected() || GetState() == DOLPHIN_NETPLAY_FAILED)
  {
    Drop();
    SetState(DOLPHIN_NETPLAY_FAILED);
    if (s_tell)
      s_tell("The online game ended before it started: playing on your own");
    return Before::Offline;
  }
  if (s_role == Role::Host && !s_start_pending)
    MaybeStart();
  return Before::Wait;
}

bool PumpRunning()
{
  if (s_role == Role::None)
    return false;
  UpdatePlayers();
  if (s_booted)
  {
    Core::System& system = Core::System::GetInstance();
    const u64 per_second = system.GetSystemTimers().GetTicksPerSecond();
    const u64 ticks = system.GetCoreTiming().GetTicks();
    std::lock_guard lock(s_status_mutex);
    s_status.game_ms = per_second ? static_cast<uint32_t>(ticks * 1000 / per_second) : 0;
  }
  if (s_role == Role::Host && s_server && s_booted)
    FollowPings();
  if (s_stop_requested.exchange(false))
  {
    SetState(DOLPHIN_NETPLAY_ENDED);
    return true;
  }
  return false;
}

void BeforeStop()
{
  if (s_client)
    s_client->InvokeStop();
}

void Shutdown()
{
  Reset();
  s_game.reset();
  s_boot = {};
  s_tell = {};
  s_game_path.clear();
}
}  // namespace TamaribaNetPlay

// --- C interface --------------------------------------------------------------------------------

using namespace TamaribaNetPlay;

extern "C" RETRO_API int dolphin_netplay_host(uint16_t* port, uint32_t seats, const char* nickname)
{
  if (!port || !Begin(Role::Host, nickname, 0))
    return -1;
  s_expected = seats | 1u;
  s_server = std::make_unique<NetPlay::NetPlayServer>(*port, false, &s_ui, LoopbackOnly());
  if (!s_server->is_connected)
  {
    Reset();
    SetState(DOLPHIN_NETPLAY_FAILED);
    return -1;
  }
  *port = s_server->GetPort();
  s_server->ChangeGame(s_game->GetSyncIdentifier(),
                       fmt::format("{} ({})", s_game->GetInternalName(), s_game->GetGameID()));
  INFO_LOG_FMT(NETPLAY, "Tamariba: hosting on 127.0.0.1:{} for seats {:#x}", *port, seats);
  Connect("127.0.0.1", *port, 15s);
  return 0;
}

extern "C" RETRO_API int dolphin_netplay_join(const char* address, uint16_t port,
                                              const char* nickname, int seat)
{
  if (!address || port == 0 || seat < 1 || seat > 9 || !Begin(Role::Guest, nickname, seat))
    return -1;
  INFO_LOG_FMT(NETPLAY, "Tamariba: joining {}:{} as seat {}", address, port, seat);
  Connect(address, port, kJoinPatience);
  return 0;
}

extern "C" RETRO_API void dolphin_netplay_expect(uint32_t seats)
{
  s_expected = seats | 1u;
}

extern "C" RETRO_API void dolphin_netplay_set_buffer(int buffer)
{
  s_buffer_override = std::clamp(buffer, 0, 40);
}

extern "C" RETRO_API void dolphin_netplay_leave(void)
{
  // Before the game starts: it then starts on its own. While it runs, NetPlay ends it.
  if (!s_booted)
  {
    Reset();
    return;
  }
  BeforeStop();
  s_stop_requested = true;
}

extern "C" RETRO_API int dolphin_netplay_get_status(dolphin_netplay_status* status)
{
  if (!status || status->version != DOLPHIN_NETPLAY_STATUS_VERSION)
    return -1;
  std::lock_guard lock(s_status_mutex);
  *status = s_status;
  status->version = DOLPHIN_NETPLAY_STATUS_VERSION;
  return 0;
}
