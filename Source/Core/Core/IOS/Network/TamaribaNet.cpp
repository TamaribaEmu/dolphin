// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Network/TamaribaNet.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <memory>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "Common/Logging/Log.h"
#include "DolphinShield/tamariba_net.h"

namespace IOS::HLE::TamaribaNet
{
namespace
{
using Clock = std::chrono::steady_clock;

std::atomic<bool> s_enabled{false};

// The interface, and its generation: sockets opened through an earlier one are dead.
std::shared_mutex s_inet_mutex;
const tamariba_inet_interface* s_inet = nullptr;
std::atomic<uint32_t> s_generation{1};

// The platform's way to say what went wrong (errno, or the WinSock error).
enum class Error
{
  Again,
  InProgress,
  Already,
  IsConn,
  NotConn,
  Refused,
  Reset,
  NetUnreach,
  BadF,
  OpNotSupp,
  AfNoSupport,
  Inval,
  NoBufs,
  MsgSize,
  ProtoNoSupport,
};

int Fail(Error e)
{
#ifdef _WIN32
  int code = WSAEINVAL;
  switch (e)
  {
  case Error::Again:
  case Error::InProgress:
    code = WSAEWOULDBLOCK;  // Dolphin reads it as EAGAIN or EINPROGRESS, by call
    break;
  case Error::Already: code = WSAEALREADY; break;
  case Error::IsConn: code = WSAEISCONN; break;
  case Error::NotConn: code = WSAENOTCONN; break;
  case Error::Refused: code = WSAECONNREFUSED; break;
  case Error::Reset: code = WSAECONNRESET; break;
  case Error::NetUnreach: code = WSAENETUNREACH; break;
  case Error::BadF: code = WSAENOTSOCK; break;
  case Error::OpNotSupp: code = WSAEOPNOTSUPP; break;
  case Error::AfNoSupport: code = WSAEAFNOSUPPORT; break;
  case Error::Inval: code = WSAEINVAL; break;
  case Error::NoBufs: code = WSAENOBUFS; break;
  case Error::MsgSize: code = WSAEMSGSIZE; break;
  case Error::ProtoNoSupport: code = WSAEPROTONOSUPPORT; break;
  }
  WSASetLastError(code);
#else
  int code = EINVAL;
  switch (e)
  {
  case Error::Again: code = EAGAIN; break;
  case Error::InProgress: code = EINPROGRESS; break;
  case Error::Already: code = EALREADY; break;
  case Error::IsConn: code = EISCONN; break;
  case Error::NotConn: code = ENOTCONN; break;
  case Error::Refused: code = ECONNREFUSED; break;
  case Error::Reset: code = ECONNRESET; break;
  case Error::NetUnreach: code = ENETUNREACH; break;
  case Error::BadF: code = EBADF; break;
  case Error::OpNotSupp: code = EOPNOTSUPP; break;
  case Error::AfNoSupport: code = EAFNOSUPPORT; break;
  case Error::Inval: code = EINVAL; break;
  case Error::NoBufs: code = ENOBUFS; break;
  case Error::MsgSize: code = EMSGSIZE; break;
  case Error::ProtoNoSupport: code = EPROTONOSUPPORT; break;
  }
  errno = code;
#endif
  return -1;
}

// SO_ERROR's value for an error.
int ErrorValue(Error e)
{
  Fail(e);
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

// Calls into the interface, if there is one (and it is still the socket's).
template <typename F>
auto WithInet(uint32_t generation, F&& f) -> decltype(f(*s_inet))
{
  std::shared_lock lock(s_inet_mutex);
  if (!s_inet || (generation != 0 && generation != s_generation.load()))
    return decltype(f(*s_inet))(TAMARIBA_INET_CLOSED);
  return f(*s_inet);
}

struct Datagram
{
  uint32_t ip = 0;  // network order
  uint16_t port = 0;  // host order
  std::vector<uint8_t> data;
};

struct VirtualSocket
{
  int type = SOCK_STREAM;
  uint32_t generation = 0;
  uint32_t local_ip = 0;     // network order
  uint16_t local_port = 0;   // host order
  uint32_t peer_ip = 0;      // network order (TCP: the connection's; UDP: connect()'s)
  uint16_t peer_port = 0;
  int pending_error = 0;     // SO_ERROR
  // TCP
  enum class State
  {
    Idle,
    Connecting,
    Connected,
    Failed,
  } state = State::Idle;
  int tcp = 0;
  bool eof = false;
  bool write_shut = false;
  std::deque<uint8_t> in;
  // UDP
  int udp = 0;
  bool udp_connected = false;
  std::deque<Datagram> datagrams;
};

constexpr size_t kMaxBuffered = 256 * 1024;
std::map<int, VirtualSocket> s_sockets;  // CPU thread only
int s_next_fd = kFirstFd;
uint16_t s_next_port = 49152;

bool IsVirtual(int fd)
{
  return fd >= kFirstFd;
}

VirtualSocket* Find(int fd)
{
  const auto it = s_sockets.find(fd);
  return it == s_sockets.end() ? nullptr : &it->second;
}

uint16_t EphemeralPort()
{
  const uint16_t port = s_next_port;
  s_next_port = s_next_port >= 65000 ? 49152 : static_cast<uint16_t>(s_next_port + 1);
  return port;
}

// Takes what arrived: a TCP connection's progress and bytes, a UDP socket's datagrams.
void Pump(VirtualSocket& s)
{
  if (s.generation != s_generation.load())
  {
    if (s.type == SOCK_STREAM && s.state != VirtualSocket::State::Failed)
    {
      s.state = VirtualSocket::State::Failed;
      s.pending_error = ErrorValue(Error::Reset);
    }
    return;
  }
  if (s.type == SOCK_STREAM)
  {
    if (s.state == VirtualSocket::State::Connecting)
    {
      const int state = WithInet(s.generation, [&](const tamariba_inet_interface& i) {
        return i.tcp_state(i.ctx, s.tcp);
      });
      if (state == TAMARIBA_INET_OK)
      {
        s.state = VirtualSocket::State::Connected;
      }
      else if (state != TAMARIBA_INET_AGAIN)
      {
        s.state = VirtualSocket::State::Failed;
        s.pending_error =
            ErrorValue(state == TAMARIBA_INET_REFUSED ? Error::Refused : Error::NetUnreach);
      }
    }
    if (s.state != VirtualSocket::State::Connected || s.eof)
      return;
    uint8_t buffer[4096];
    while (s.in.size() < kMaxBuffered)
    {
      const int got = WithInet(s.generation, [&](const tamariba_inet_interface& i) {
        return i.tcp_recv(i.ctx, s.tcp, buffer, sizeof(buffer));
      });
      if (got > 0)
      {
        s.in.insert(s.in.end(), buffer, buffer + got);
        continue;
      }
      if (got == TAMARIBA_INET_CLOSED || got == 0)
        s.eof = true;
      else if (got != TAMARIBA_INET_AGAIN)
      {
        s.state = VirtualSocket::State::Failed;
        s.pending_error = ErrorValue(Error::Reset);
      }
      break;
    }
    return;
  }
  if (s.udp <= 0)
    return;
  uint8_t buffer[2048];
  while (s.datagrams.size() < 256)
  {
    uint32_t ip = 0;
    uint16_t port = 0;
    const int got = WithInet(s.generation, [&](const tamariba_inet_interface& i) {
      return i.udp_recvfrom(i.ctx, s.udp, &ip, &port, buffer, sizeof(buffer));
    });
    if (got < 0)
      break;
    // A connected UDP socket takes only its peer's datagrams.
    if (s.udp_connected && (ip != s.peer_ip || port != s.peer_port))
      continue;
    s.datagrams.push_back({ip, port, std::vector<uint8_t>(buffer, buffer + got)});
  }
}

void FillAddress(sockaddr* addr, socklen_t* len, uint32_t ip, uint16_t port)
{
  if (!addr || !len)
    return;
  sockaddr_in in{};
  in.sin_family = AF_INET;
  in.sin_addr.s_addr = ip;
  in.sin_port = htons(port);
  std::memcpy(addr, &in, std::min<size_t>(*len, sizeof(in)));
  *len = sizeof(in);
}

bool ReadAddress(const sockaddr* addr, socklen_t len, uint32_t* ip, uint16_t* port)
{
  if (!addr || len < static_cast<socklen_t>(sizeof(sockaddr_in)))
    return false;
  sockaddr_in in;
  std::memcpy(&in, addr, sizeof(in));
  *ip = in.sin_addr.s_addr;
  *port = ntohs(in.sin_port);
  return true;
}

// The UDP socket's handle at the exit, opened on first use.
bool OpenUdp(VirtualSocket& s)
{
  if (s.udp > 0)
    return true;
  if (s.local_port == 0)
    s.local_port = EphemeralPort();
  s.udp = WithInet(s.generation, [&](const tamariba_inet_interface& i) {
    return i.udp_open(i.ctx, s.local_port);
  });
  return s.udp > 0;
}

bool IsPublicDestination(uint32_t ip)
{
  const uint32_t h = ntohl(ip);
  return h != 0 && h != 0xFFFFFFFF && (h >> 24) != 127 && (h >> 24) != 10 &&
         (h >> 20) != (172 << 4 | 1) && (h >> 16) != (192 << 8 | 168) && (h >> 28) != 0xE;
}

// The lookup, waiting for the Plaza's answer (the CPU thread waits, as on a Wii).
bool Resolve(const char* name, uint32_t* ip)
{
  in_addr numeric{};
  if (inet_pton(AF_INET, name, &numeric) == 1)
  {
    *ip = numeric.s_addr;
    return true;
  }
  const uint32_t generation = s_generation.load();
  const int handle = WithInet(generation, [&](const tamariba_inet_interface& i) {
    return i.resolve(i.ctx, name);
  });
  if (handle <= 0)
    return false;
  const Clock::time_point deadline = Clock::now() + std::chrono::seconds(15);
  while (Clock::now() < deadline)
  {
    const int result = WithInet(generation, [&](const tamariba_inet_interface& i) {
      return i.resolve_result(i.ctx, handle, ip);
    });
    if (result != TAMARIBA_INET_AGAIN)
      return result == TAMARIBA_INET_OK;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}
}  // namespace

void SetEnabled(bool enabled)
{
  s_enabled = enabled;
}

bool Enabled()
{
  return s_enabled;
}

void SetInterface(const tamariba_inet_interface* inet)
{
  std::unique_lock lock(s_inet_mutex);  // no call into the old one is under way after this
  s_inet = inet && inet->version >= 1 ? inet : nullptr;
  s_generation.fetch_add(1);
  INFO_LOG_FMT(IOS_NET, "Tamariba: the console's network {}",
               s_inet ? "goes through the Plaza's exit" : "is off");
}

uint32_t PublicIPv4()
{
  std::shared_lock lock(s_inet_mutex);
  return s_inet ? s_inet->public_ipv4(s_inet->ctx) : 0;
}

int Socket(int af, int type, int protocol)
{
  if (!Enabled())
    return static_cast<int>(::socket(af, type, protocol));
  if (af != AF_INET)
    return Fail(Error::AfNoSupport);
  if (type != SOCK_STREAM && type != SOCK_DGRAM)
    return Fail(Error::ProtoNoSupport);  // raw (ICMP) sockets: none
  {
    std::shared_lock lock(s_inet_mutex);
    if (!s_inet)
      return Fail(Error::NetUnreach);  // no network for the console
  }
  VirtualSocket s;
  s.type = type;
  s.generation = s_generation.load();
  const int fd = s_next_fd++;
  s_sockets.emplace(fd, std::move(s));
  return fd;
}

int Close(int fd)
{
  if (!IsVirtual(fd))
  {
#ifdef _WIN32
    return closesocket(fd);
#else
    return close(fd);
#endif
  }
  VirtualSocket* s = Find(fd);
  if (!s)
    return Fail(Error::BadF);
  WithInet(s->generation, [&](const tamariba_inet_interface& i) {
    if (s->tcp > 0)
      i.tcp_close(i.ctx, s->tcp);
    if (s->udp > 0)
      i.udp_close(i.ctx, s->udp);
    return 0;
  });
  s_sockets.erase(fd);
  return 0;
}

void SetNonBlocking(int fd)
{
  if (IsVirtual(fd))
    return;  // always
#ifdef _WIN32
  u_long mode = 1;
  ioctlsocket(fd, FIONBIO, &mode);
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    flags = 0;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

int Bind(int fd, const sockaddr* addr, socklen_t len)
{
  if (!IsVirtual(fd))
    return ::bind(fd, addr, len);
  VirtualSocket* s = Find(fd);
  if (!s)
    return Fail(Error::BadF);
  uint32_t ip = 0;
  uint16_t port = 0;
  if (!ReadAddress(addr, len, &ip, &port))
    return Fail(Error::Inval);
  s->local_port = port ? port : EphemeralPort();
  if (s->type == SOCK_DGRAM && !OpenUdp(*s))
    return Fail(Error::NetUnreach);
  return 0;
}

int Connect(int fd, const sockaddr* addr, socklen_t len)
{
  if (!IsVirtual(fd))
    return ::connect(fd, addr, len);
  VirtualSocket* s = Find(fd);
  if (!s)
    return Fail(Error::BadF);
  uint32_t ip = 0;
  uint16_t port = 0;
  if (!ReadAddress(addr, len, &ip, &port))
    return Fail(Error::Inval);
  if (s->type == SOCK_DGRAM)
  {
    s->peer_ip = ip;
    s->peer_port = port;
    s->udp_connected = ip != 0;
    return OpenUdp(*s) ? 0 : Fail(Error::NetUnreach);
  }
  Pump(*s);
  switch (s->state)
  {
  case VirtualSocket::State::Connected:
    return Fail(Error::IsConn);
  case VirtualSocket::State::Connecting:
    return Fail(Error::Already);
  case VirtualSocket::State::Failed:
  {
    const int error = s->pending_error;
    s->pending_error = 0;
    s->state = VirtualSocket::State::Idle;  // (it may try again)
#ifdef _WIN32
    WSASetLastError(error);
#else
    errno = error;
#endif
    return -1;
  }
  case VirtualSocket::State::Idle:
    break;
  }
  if (!IsPublicDestination(ip))
    return Fail(Error::NetUnreach);  // the console's network is the internet, nothing else
  const int handle = WithInet(s->generation, [&](const tamariba_inet_interface& i) {
    return i.tcp_connect(i.ctx, ip, port);
  });
  if (handle <= 0)
    return Fail(handle == TAMARIBA_INET_REFUSED ? Error::Refused : Error::NetUnreach);
  s->tcp = handle;
  s->peer_ip = ip;
  s->peer_port = port;
  if (s->local_port == 0)
    s->local_port = EphemeralPort();
  s->state = VirtualSocket::State::Connecting;
  Pump(*s);
  if (s->state == VirtualSocket::State::Connected)
    return 0;
  return Fail(Error::InProgress);
}

int Listen(int fd, int backlog)
{
  if (!IsVirtual(fd))
    return ::listen(fd, backlog);
  return Fail(Error::OpNotSupp);  // the console only connects out (no listen through the exit)
}

int Accept(int fd, sockaddr* addr, socklen_t* len)
{
  if (!IsVirtual(fd))
    return static_cast<int>(::accept(fd, addr, len));
  return Fail(Error::OpNotSupp);
}

int Shutdown(int fd, int how)
{
  if (!IsVirtual(fd))
    return ::shutdown(fd, how);
  VirtualSocket* s = Find(fd);
  if (!s)
    return Fail(Error::BadF);
  if (s->type == SOCK_STREAM && s->state != VirtualSocket::State::Connected)
    return Fail(Error::NotConn);
  if (how == 1 || how == 2)
    s->write_shut = true;
  if (how == 0 || how == 2)
  {
    s->in.clear();
    s->eof = true;
  }
  return 0;
}

int SendTo(int fd, const char* data, size_t size, int flags, const sockaddr* to, socklen_t len)
{
  if (!IsVirtual(fd))
    return static_cast<int>(::sendto(fd, data, static_cast<int>(size), flags, to, len));
  VirtualSocket* s = Find(fd);
  if (!s)
    return Fail(Error::BadF);
  Pump(*s);
  if (s->type == SOCK_STREAM)
  {
    if (s->state == VirtualSocket::State::Connecting)
      return Fail(Error::Again);
    if (s->state != VirtualSocket::State::Connected)
      return Fail(s->state == VirtualSocket::State::Failed ? Error::Reset : Error::NotConn);
    if (s->write_shut)
      return Fail(Error::NotConn);
    if (size == 0)
      return 0;
    const int sent = WithInet(s->generation, [&](const tamariba_inet_interface& i) {
      return i.tcp_send(i.ctx, s->tcp, data, size);
    });
    if (sent >= 0)
      return sent;
    if (sent == TAMARIBA_INET_AGAIN)
      return Fail(Error::Again);
    s->state = VirtualSocket::State::Failed;
    return Fail(Error::Reset);
  }
  uint32_t ip = s->peer_ip;
  uint16_t port = s->peer_port;
  if (to && !ReadAddress(to, len, &ip, &port))
    return Fail(Error::Inval);
  if (!to && !s->udp_connected)
    return Fail(Error::NotConn);
  if (size > 1472)
    return Fail(Error::MsgSize);
  if (!IsPublicDestination(ip))
    return static_cast<int>(size);  // a broadcast or a LAN address: there is no LAN; gone
  if (!OpenUdp(*s))
    return Fail(Error::NetUnreach);
  const int sent = WithInet(s->generation, [&](const tamariba_inet_interface& i) {
    return i.udp_sendto(i.ctx, s->udp, ip, port, data, size);
  });
  if (sent >= 0)
    return static_cast<int>(size);
  return Fail(sent == TAMARIBA_INET_AGAIN ? Error::NoBufs : Error::NetUnreach);
}

int RecvFrom(int fd, char* data, size_t size, int flags, sockaddr* from, socklen_t* len)
{
  if (!IsVirtual(fd))
    return static_cast<int>(::recvfrom(fd, data, static_cast<int>(size), flags, from, len));
  VirtualSocket* s = Find(fd);
  if (!s)
    return Fail(Error::BadF);
  Pump(*s);
  const bool peek = (flags & MSG_PEEK) != 0;
  if (s->type == SOCK_STREAM)
  {
    if (s->state == VirtualSocket::State::Connecting)
      return Fail(Error::Again);
    if (s->state == VirtualSocket::State::Failed && s->in.empty())
      return Fail(Error::Reset);
    if (s->state == VirtualSocket::State::Idle)
      return Fail(Error::NotConn);
    if (s->in.empty())
      return s->eof ? 0 : Fail(Error::Again);
    const size_t n = std::min(size, s->in.size());
    std::copy_n(s->in.begin(), n, data);
    if (!peek)
      s->in.erase(s->in.begin(), s->in.begin() + static_cast<std::ptrdiff_t>(n));
    FillAddress(from, len, s->peer_ip, s->peer_port);
    return static_cast<int>(n);
  }
  if (s->datagrams.empty())
    return Fail(Error::Again);
  const Datagram& d = s->datagrams.front();
  const size_t n = std::min(size, d.data.size());
  std::memcpy(data, d.data.data(), n);
  FillAddress(from, len, d.ip, d.port);
  if (!peek)
    s->datagrams.pop_front();
  return static_cast<int>(n);
}

int GetSockOpt(int fd, int level, int name, char* value, socklen_t* len)
{
  if (!IsVirtual(fd))
    return ::getsockopt(fd, level, name, value, len);
  VirtualSocket* s = Find(fd);
  if (!s)
    return Fail(Error::BadF);
  int result = 0;
  if (level == SOL_SOCKET && name == SO_TYPE)
  {
    result = s->type;
  }
  else if (level == SOL_SOCKET && name == SO_ERROR)
  {
    Pump(*s);
    result = s->pending_error;
    s->pending_error = 0;
  }
  if (value && len && *len >= static_cast<socklen_t>(sizeof(int)))
  {
    std::memcpy(value, &result, sizeof(int));
    *len = sizeof(int);
  }
  return 0;
}

int SetSockOpt(int fd, int level, int name, const char* value, socklen_t len)
{
  if (!IsVirtual(fd))
    return ::setsockopt(fd, level, name, value, len);
  return Find(fd) ? 0 : Fail(Error::BadF);  // nothing to tune: the exit has its own sockets
}

int GetSockName(int fd, sockaddr* addr, socklen_t* len)
{
  if (!IsVirtual(fd))
    return ::getsockname(fd, addr, len);
  VirtualSocket* s = Find(fd);
  if (!s)
    return Fail(Error::BadF);
  FillAddress(addr, len, s->local_ip, s->local_port);
  return 0;
}

int GetPeerName(int fd, sockaddr* addr, socklen_t* len)
{
  if (!IsVirtual(fd))
    return ::getpeername(fd, addr, len);
  VirtualSocket* s = Find(fd);
  if (!s)
    return Fail(Error::BadF);
  Pump(*s);
  const bool connected = s->type == SOCK_STREAM ? s->state == VirtualSocket::State::Connected :
                                                  s->udp_connected;
  if (!connected)
    return Fail(Error::NotConn);
  FillAddress(addr, len, s->peer_ip, s->peer_port);
  return 0;
}

int Available(int fd)
{
  if (!IsVirtual(fd))
  {
#ifdef _WIN32
    u_long count = 0;
    ioctlsocket(fd, FIONREAD, &count);
    return static_cast<int>(count);
#else
    int count = 0;
    ioctl(fd, FIONREAD, &count);
    return count;
#endif
  }
  VirtualSocket* s = Find(fd);
  if (!s)
    return 0;
  Pump(*s);
  if (s->type == SOCK_STREAM)
    return static_cast<int>(s->in.size());
  return s->datagrams.empty() ? 0 : static_cast<int>(s->datagrams.front().data.size());
}

Ready Readiness(int fd)
{
  Ready r;
  VirtualSocket* s = Find(fd);
  if (!s)
  {
    r.error = true;
    return r;
  }
  Pump(*s);
  if (s->type == SOCK_STREAM)
  {
    r.read = !s->in.empty() || s->eof || s->state == VirtualSocket::State::Failed;
    r.write = s->state == VirtualSocket::State::Connected ||
              s->state == VirtualSocket::State::Failed;
    r.error = s->state == VirtualSocket::State::Failed;
  }
  else
  {
    r.read = !s->datagrams.empty();
    r.write = true;
  }
  return r;
}

int Poll(PollFd* fds, size_t count)
{
  int ready = 0;
  for (size_t i = 0; i < count; ++i)
  {
    PollFd& p = fds[i];
    p.revents = 0;
    if (!IsVirtual(static_cast<int>(p.fd)) || !Find(static_cast<int>(p.fd)))
    {
      p.revents = POLLNVAL;
    }
    else
    {
      const Ready r = Readiness(static_cast<int>(p.fd));
      if (r.read)
        p.revents |= p.events & (POLLIN | POLLRDNORM);
      if (r.write)
        p.revents |= p.events & (POLLOUT | POLLWRNORM);
      if (r.error)
        p.revents |= POLLERR;
    }
    if (p.revents)
      ++ready;
  }
  return ready;
}

hostent* GetHostByName(const char* name)
{
  if (!Enabled())
    return ::gethostbyname(name);
  thread_local std::string host_name;
  thread_local uint32_t address;
  thread_local char* addresses[2];
  thread_local char* aliases[1];
  thread_local hostent entry;
  if (!name || !Resolve(name, &address))
    return nullptr;
  host_name = name;
  addresses[0] = reinterpret_cast<char*>(&address);
  addresses[1] = nullptr;
  aliases[0] = nullptr;
  entry = {};
  entry.h_name = host_name.data();
  entry.h_aliases = aliases;
  entry.h_addrtype = AF_INET;
  entry.h_length = sizeof(uint32_t);
  entry.h_addr_list = addresses;
  return &entry;
}

namespace
{
struct OwnedAddrInfo
{
  addrinfo info{};
  sockaddr_in address{};
};
}  // namespace

int GetAddrInfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result)
{
  if (!Enabled())
    return ::getaddrinfo(node, service, hints, result);
  *result = nullptr;
  if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
    return EAI_FAMILY;
  uint32_t ip = htonl(INADDR_LOOPBACK);
  if (node && !Resolve(node, &ip))
    return EAI_NONAME;
  const uint16_t port = service ? static_cast<uint16_t>(std::atoi(service)) : 0;
  auto* owned = new OwnedAddrInfo;
  owned->address.sin_family = AF_INET;
  owned->address.sin_addr.s_addr = ip;
  owned->address.sin_port = htons(port);
  owned->info.ai_family = AF_INET;
  owned->info.ai_socktype = hints ? hints->ai_socktype : 0;
  owned->info.ai_protocol = hints ? hints->ai_protocol : 0;
  owned->info.ai_addrlen = sizeof(sockaddr_in);
  owned->info.ai_addr = reinterpret_cast<sockaddr*>(&owned->address);
  *result = &owned->info;
  return 0;
}

void FreeAddrInfo(addrinfo* result)
{
  if (!Enabled())
  {
    ::freeaddrinfo(result);
    return;
  }
  delete reinterpret_cast<OwnedAddrInfo*>(result);  // info is its first member
}

int MbedSend(void* ctx, const unsigned char* buf, size_t len)
{
  const int fd = *static_cast<int*>(ctx);
  if (!IsVirtual(fd))
    return mbedtls_net_send(ctx, buf, len);
  const int sent = SendTo(fd, reinterpret_cast<const char*>(buf), len, 0, nullptr, 0);
  if (sent >= 0)
    return sent;
  const Ready r = Readiness(fd);
  return r.error ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_SSL_WANT_WRITE;
}

int MbedRecv(void* ctx, unsigned char* buf, size_t len)
{
  const int fd = *static_cast<int*>(ctx);
  if (!IsVirtual(fd))
    return mbedtls_net_recv(ctx, buf, len);
  const int got = RecvFrom(fd, reinterpret_cast<char*>(buf), len, 0, nullptr, nullptr);
  if (got >= 0)
    return got;  // 0: the other side closed
  const Ready r = Readiness(fd);
  return r.error ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_SSL_WANT_READ;
}
}  // namespace IOS::HLE::TamaribaNet
