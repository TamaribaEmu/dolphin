// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the Wii's network through Tamariba's internet exit (Core/IOS/Network/TamaribaNet.h)
// against a stand-in exit in memory: no host socket is ever opened, a lookup is the exit's
// answer, TCP and UDP behave like the BSD calls the IOS code expects, and without an exit the
// console has no network at all. Build and run: the dolphin_tamariba_net_test target (not
// built by default); it prints "ok" per test and exits non-zero on a failure.

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#endif

#include "Common/FileUtil.h"
#include "Core/IOS/Network/TamaribaNet.h"
#include "Core/TamaribaWFC.h"
#include "DolphinShield/tamariba_net.h"

namespace Net = IOS::HLE::TamaribaNet;

namespace
{
int s_failures = 0;
#define CHECK(x)                                                                                   \
  do                                                                                               \
  {                                                                                                \
    if (!(x))                                                                                      \
    {                                                                                              \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x);                                   \
      ++s_failures;                                                                                \
    }                                                                                              \
  } while (0)

int LastError()
{
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

// The stand-in exit: names from a table; TCP connections to 93.184.216.34:80 echo what they
// get (after one "not yet" while connecting); UDP datagrams come back from where they went.
struct FakeExit
{
  struct Tcp
  {
    int polls_until_connected = 1;
    std::deque<unsigned char> to_console;
    bool closed_by_peer = false;
  };
  struct Udp
  {
    uint16_t port = 0;
    std::deque<std::pair<std::pair<uint32_t, uint16_t>, std::vector<unsigned char>>> to_console;
  };
  std::map<std::string, uint32_t> names;
  std::map<int, uint32_t> lookups;
  std::map<int, Tcp> tcp;
  std::map<int, Udp> udp;
  int next = 1;
  int udp_sent = 0;
  std::vector<std::pair<uint32_t, uint16_t>> connects;
  tamariba_inet_interface iface{};

  static FakeExit& Self(void* ctx) { return *static_cast<FakeExit*>(ctx); }

  FakeExit()
  {
    names["naswii.nintendowifi.net"] = inet_addr("93.184.216.34");
    iface.version = TAMARIBA_NET_VERSION;
    iface.ctx = this;
    iface.public_ipv4 = [](void*) -> uint32_t { return inet_addr("203.0.113.7"); };
    iface.resolve = [](void* ctx, const char* host) {
      FakeExit& f = Self(ctx);
      const auto it = f.names.find(host);
      const int h = f.next++;
      f.lookups[h] = it == f.names.end() ? 0 : it->second;
      return h;
    };
    iface.resolve_result = [](void* ctx, int handle, uint32_t* ip) {
      FakeExit& f = Self(ctx);
      const uint32_t found = f.lookups[handle];
      f.lookups.erase(handle);
      if (!found)
        return static_cast<int>(TAMARIBA_INET_ERROR);
      *ip = found;
      return static_cast<int>(TAMARIBA_INET_OK);
    };
    iface.tcp_connect = [](void* ctx, uint32_t ip, uint16_t port) {
      FakeExit& f = Self(ctx);
      f.connects.emplace_back(ip, port);
      if (port != 80)
        return static_cast<int>(TAMARIBA_INET_REFUSED);
      const int h = f.next++;
      f.tcp[h] = {};
      return h;
    };
    iface.tcp_state = [](void* ctx, int handle) {
      Tcp& t = Self(ctx).tcp.at(handle);
      return t.polls_until_connected-- > 0 ? static_cast<int>(TAMARIBA_INET_AGAIN) :
                                             static_cast<int>(TAMARIBA_INET_OK);
    };
    iface.tcp_send = [](void* ctx, int handle, const void* data, size_t size) {
      Tcp& t = Self(ctx).tcp.at(handle);
      const auto* p = static_cast<const unsigned char*>(data);
      t.to_console.insert(t.to_console.end(), p, p + size);  // an echo
      return static_cast<int>(size);
    };
    iface.tcp_recv = [](void* ctx, int handle, void* buf, size_t cap) {
      Tcp& t = Self(ctx).tcp.at(handle);
      if (t.to_console.empty())
        return t.closed_by_peer ? static_cast<int>(TAMARIBA_INET_CLOSED) :
                                  static_cast<int>(TAMARIBA_INET_AGAIN);
      const size_t n = std::min(cap, t.to_console.size());
      std::copy_n(t.to_console.begin(), n, static_cast<unsigned char*>(buf));
      t.to_console.erase(t.to_console.begin(), t.to_console.begin() + n);
      return static_cast<int>(n);
    };
    iface.tcp_close = [](void* ctx, int handle) { Self(ctx).tcp.erase(handle); };
    iface.udp_open = [](void* ctx, uint16_t port) {
      FakeExit& f = Self(ctx);
      const int h = f.next++;
      f.udp[h].port = port;
      return h;
    };
    iface.udp_sendto = [](void* ctx, int handle, uint32_t ip, uint16_t port, const void* data,
                          size_t size) {
      FakeExit& f = Self(ctx);
      ++f.udp_sent;
      const auto* p = static_cast<const unsigned char*>(data);
      f.udp.at(handle).to_console.push_back({{ip, port}, std::vector<unsigned char>(p, p + size)});
      return static_cast<int>(size);
    };
    iface.udp_recvfrom = [](void* ctx, int handle, uint32_t* ip, uint16_t* port, void* buf,
                            size_t cap) {
      Udp& u = Self(ctx).udp.at(handle);
      if (u.to_console.empty())
        return static_cast<int>(TAMARIBA_INET_AGAIN);
      auto d = std::move(u.to_console.front());
      u.to_console.pop_front();
      *ip = d.first.first;
      *port = d.first.second;
      const size_t n = std::min(cap, d.second.size());
      std::memcpy(buf, d.second.data(), n);
      return static_cast<int>(n);
    };
    iface.udp_close = [](void* ctx, int handle) { Self(ctx).udp.erase(handle); };
  }
};

sockaddr_in Address(const char* ip, uint16_t port)
{
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = inet_addr(ip);
  a.sin_port = htons(port);
  return a;
}

const sockaddr* Sa(const sockaddr_in& a)
{
  return reinterpret_cast<const sockaddr*>(&a);
}

void WithoutAnExitTheConsoleHasNoNetwork()
{
  Net::SetEnabled(true);
  Net::SetInterface(nullptr);
  CHECK(Net::Socket(AF_INET, SOCK_STREAM, 0) == -1);
  CHECK(Net::GetHostByName("naswii.nintendowifi.net") == nullptr);
  addrinfo* result = nullptr;
  CHECK(Net::GetAddrInfo("naswii.nintendowifi.net", "443", nullptr, &result) != 0 && !result);
  std::printf("ok   without an exit the console has no network\n");
}

void TcpGoesThroughTheExitWithNonBlockingConnectAndEcho()
{
  FakeExit exit;
  Net::SetEnabled(true);
  Net::SetInterface(&exit.iface);
  hostent* host = Net::GetHostByName("naswii.nintendowifi.net");
  CHECK(host && host->h_addrtype == AF_INET && host->h_addr_list[0] &&
        *reinterpret_cast<uint32_t*>(host->h_addr_list[0]) == inet_addr("93.184.216.34"));
  CHECK(Net::GetHostByName("nowhere.example") == nullptr);
  CHECK(Net::Socket(AF_INET, 3 /* SOCK_RAW */, 1) == -1);

  const int fd = Net::Socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= Net::kFirstFd);
  int type = 0;
  socklen_t len = sizeof(type);
  CHECK(Net::GetSockOpt(fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &len) == 0 &&
        type == SOCK_STREAM);
  // A LAN address is not the internet: refused at once, never asked of the exit.
  const sockaddr_in lan = Address("192.168.1.10", 80);
  CHECK(Net::Connect(fd, Sa(lan), sizeof(lan)) == -1 && exit.connects.empty());
  const sockaddr_in server = Address("93.184.216.34", 80);
  // Non-blocking: in progress, then writable once the exit has connected, and connect() then
  // says EISCONN (which Dolphin's blocking connect loop takes as done).
  CHECK(Net::Connect(fd, Sa(server), sizeof(server)) == -1 && !Net::Readiness(fd).error);
  CHECK(Net::Readiness(fd).write);
  CHECK(Net::Connect(fd, Sa(server), sizeof(server)) == -1);
  sockaddr_in peer{};
  len = sizeof(peer);
  CHECK(Net::GetPeerName(fd, reinterpret_cast<sockaddr*>(&peer), &len) == 0 &&
        peer.sin_addr.s_addr == server.sin_addr.s_addr && ntohs(peer.sin_port) == 80);
  char buffer[64] = {};
  CHECK(Net::RecvFrom(fd, buffer, sizeof(buffer), 0, nullptr, nullptr) == -1);  // nothing yet
  CHECK(Net::SendTo(fd, "GET /ac", 7, 0, nullptr, 0) == 7);
  CHECK(Net::Readiness(fd).read && Net::Available(fd) == 7);
  CHECK(Net::RecvFrom(fd, buffer, 3, MSG_PEEK, nullptr, nullptr) == 3);
  CHECK(Net::RecvFrom(fd, buffer, sizeof(buffer), 0, nullptr, nullptr) == 7 &&
        std::memcmp(buffer, "GET /ac", 7) == 0);
  // The other side closes: a read of 0.
  exit.tcp.begin()->second.closed_by_peer = true;
  CHECK(Net::RecvFrom(fd, buffer, sizeof(buffer), 0, nullptr, nullptr) == 0);
  CHECK(Net::Close(fd) == 0 && exit.tcp.empty());
  // A refused connection: the error arrives at once.
  const int refused = Net::Socket(AF_INET, SOCK_STREAM, 0);
  const sockaddr_in closed_port = Address("93.184.216.34", 25);
  CHECK(Net::Connect(refused, Sa(closed_port), sizeof(closed_port)) == -1);
  Net::Close(refused);
  std::printf("ok   tcp goes through the exit with a non-blocking connect\n");
}

void UdpGoesThroughTheExitAndNothingReachesALan()
{
  FakeExit exit;
  Net::SetEnabled(true);
  Net::SetInterface(&exit.iface);
  const int fd = Net::Socket(AF_INET, SOCK_DGRAM, 0);
  const sockaddr_in any = Address("0.0.0.0", 0);
  CHECK(Net::Bind(fd, Sa(any), sizeof(any)) == 0 && exit.udp.size() == 1);
  const sockaddr_in peer = Address("198.51.100.20", 3074);
  CHECK(Net::SendTo(fd, "hello", 5, 0, Sa(peer), sizeof(peer)) == 5 && exit.udp_sent == 1);
  // A broadcast (looking for players on the LAN): there is no LAN; it goes nowhere.
  const sockaddr_in broadcast = Address("255.255.255.255", 3074);
  CHECK(Net::SendTo(fd, "anyone", 6, 0, Sa(broadcast), sizeof(broadcast)) == 6 &&
        exit.udp_sent == 1);
  sockaddr_in from{};
  socklen_t len = sizeof(from);
  char buffer[16] = {};
  CHECK(Net::RecvFrom(fd, buffer, sizeof(buffer), MSG_PEEK, reinterpret_cast<sockaddr*>(&from),
                      &len) == 5);
  CHECK(Net::RecvFrom(fd, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &len) ==
            5 &&
        std::memcmp(buffer, "hello", 5) == 0 && from.sin_addr.s_addr == peer.sin_addr.s_addr &&
        ntohs(from.sin_port) == 3074);
  CHECK(Net::RecvFrom(fd, buffer, sizeof(buffer), 0, nullptr, nullptr) == -1);
  // poll(): readable once something waits, always writable.
  Net::PollFd p{};
  p.fd = fd;
  p.events = POLLIN | POLLOUT;
  CHECK(Net::Poll(&p, 1) == 1 && (p.revents & POLLOUT) && !(p.revents & POLLIN));
  // The exit goes away: the console's sockets are dead, new ones can't be made.
  Net::SetInterface(nullptr);
  CHECK(Net::SendTo(fd, "x", 1, 0, Sa(peer), sizeof(peer)) == -1);
  Net::Close(fd);
  CHECK(Net::Socket(AF_INET, SOCK_DGRAM, 0) == -1);
  std::printf("ok   udp goes through the exit and nothing reaches a LAN\n");
}

void TcpFromAnEarlierExitReadsAsReset()
{
  FakeExit first, second;
  Net::SetEnabled(true);
  Net::SetInterface(&first.iface);
  const int fd = Net::Socket(AF_INET, SOCK_STREAM, 0);
  const sockaddr_in server = Address("93.184.216.34", 80);
  Net::Connect(fd, Sa(server), sizeof(server));
  Net::Readiness(fd);
  Net::SetInterface(&second.iface);  // the exit reconnected as another session
  const Net::Ready r = Net::Readiness(fd);
  CHECK(r.error);
  char buffer[4];
  CHECK(Net::RecvFrom(fd, buffer, sizeof(buffer), 0, nullptr, nullptr) == -1);
  int error = 0;
  socklen_t len = sizeof(error);
  CHECK(Net::GetSockOpt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) == 0);
  Net::Close(fd);
  CHECK(second.tcp.empty() && second.connects.empty());
  (void)LastError;
  std::printf("ok   tcp from an earlier exit reads as reset\n");
}
void WiiLinkPatchesAreFoundByGameIdAndRevision()
{
  const std::string text = "# a comment\n"
                           "[RMCED00]\n220EDDE4 4800007C\n060EDFF8 00000058\n"
                           "[RMCPD00]\n220EDE84 4800007C\n"
                           "[WABEN0001]\n04001234 60000000\n";
  const auto mkwii = TamaribaWFC::PatchFromText(text, "RMCE01", 0);
  CHECK(mkwii && mkwii->enabled && mkwii->codes.size() == 2 && mkwii->codes[0].address == 0x220EDDE4 &&
        mkwii->codes[1].data == 0x00000058);
  const auto pal = TamaribaWFC::PatchFromText(text, "RMCP01", 0);
  CHECK(pal && pal->codes.size() == 1 && pal->codes[0].address == 0x220EDE84);
  CHECK(!TamaribaWFC::PatchFromText(text, "RMCE01", 1));  // another revision: no patch
  CHECK(!TamaribaWFC::PatchFromText(text, "RSBE01", 0));
  const auto channel = TamaribaWFC::PatchFromText(text, "WABE", 1);
  CHECK(channel && channel->codes.size() == 1);
  CHECK(!TamaribaWFC::PatchFromText("[RMCED00]\n220EDDE4 nothex\n", "RMCE01", 0));
  CHECK(TamaribaWFC::ParseService("wiilink") == TamaribaWFC::Service::WiiLink &&
        TamaribaWFC::ParseService("wiimmfi") == TamaribaWFC::Service::Wiimmfi &&
        TamaribaWFC::ParseService("") == TamaribaWFC::Service::Off);
  // The shipped file: every section parses, Mario Kart Wii (NTSC-U) included.
  std::string shipped;
  CHECK(File::ReadFileToString(TAMARIBA_SYS_DIR "/TamaribaWFC/WiiLink.txt", shipped));
  const auto shipped_mkwii = TamaribaWFC::PatchFromText(shipped, "RMCE01", 0);
  CHECK(shipped_mkwii && shipped_mkwii->codes.size() > 20 &&
        shipped_mkwii->codes[0].address == 0x220EDDE4);
  std::istringstream sections(shipped);
  int titles = 0, parsed = 0;
  for (std::string line; std::getline(sections, line);)
  {
    if (line.size() < 9 || line[0] != '[')
      continue;
    ++titles;
    const std::string title = line.substr(1, line.size() - 2);
    const bool disc = title[4] == 'D';
    const u16 revision = static_cast<u16>(std::stoi(title.substr(5)));
    parsed += TamaribaWFC::PatchFromText(shipped, title.substr(0, 4) + (disc ? "01" : ""), revision) ? 1 : 0;
  }
  CHECK(titles > 500 && parsed == titles);
  // Off (or Wiimmfi): no patch at all.
  TamaribaWFC::SetService(TamaribaWFC::Service::Off);
  CHECK(!TamaribaWFC::PatchFor("RMCE01", 0));
  std::printf("ok   wiilink patches are found by game id and revision\n");
}
}  // namespace

int main()
{
  WiiLinkPatchesAreFoundByGameIdAndRevision();
  WithoutAnExitTheConsoleHasNoNetwork();
  TcpGoesThroughTheExitWithNonBlockingConnectAndEcho();
  UdpGoesThroughTheExitAndNothingReachesALan();
  TcpFromAnEarlierExitReadsAsReset();
  std::printf("%d failed\n", s_failures);
  return s_failures == 0 ? 0 : 1;
}
