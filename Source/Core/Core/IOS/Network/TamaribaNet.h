// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tamariba: the Wii's network (IOS sockets, name lookups and SSL) through the Tamariba Plaza's
// internet exit instead of the host's network (DolphinShield/tamariba_net.h,
// tamariba_inet_interface; docs/online-play-design-2026-10-06.md in Tamariba, "The Plaza
// internet exit"). So nobody a game meets online learns the player's address.
//
// Once Tamariba's core has called SetEnabled(true), the console never opens a host socket:
// every socket is a virtual one (a number from kFirstFd up) whose traffic goes through the
// interface, and without an interface the console simply has no network (as a Wii without
// one). Otherwise (Dolphin's other front ends) every function here is the host's own.
//
// The functions mirror the BSD calls they stand for: -1 with the platform's socket error set
// (errno, or WSASetLastError), so the IOS code reads errors as it always has. The virtual
// sockets are non-blocking and live on the CPU thread (where IOS runs); SetInterface may be
// called from any thread, and a socket opened through an earlier interface then reads as
// reset.

#pragma once

#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#endif

struct tamariba_inet_interface;

namespace IOS::HLE::TamaribaNet
{
#ifdef _WIN32
using PollFd = WSAPOLLFD;
#else
using PollFd = pollfd;
#endif

constexpr int kFirstFd = 0x10000;

void SetEnabled(bool enabled);
bool Enabled();
void SetInterface(const tamariba_inet_interface* inet);
// The address the console reports as its own (the exit's public one), network order; 0 unknown.
uint32_t PublicIPv4();

int Socket(int af, int type, int protocol);
int Close(int fd);
void SetNonBlocking(int fd);
int Bind(int fd, const sockaddr* addr, socklen_t len);
int Connect(int fd, const sockaddr* addr, socklen_t len);
int Listen(int fd, int backlog);
int Accept(int fd, sockaddr* addr, socklen_t* len);
int Shutdown(int fd, int how);
int SendTo(int fd, const char* data, size_t size, int flags, const sockaddr* to, socklen_t len);
int RecvFrom(int fd, char* data, size_t size, int flags, sockaddr* from, socklen_t* len);
int GetSockOpt(int fd, int level, int name, char* value, socklen_t* len);
int SetSockOpt(int fd, int level, int name, const char* value, socklen_t len);
int GetSockName(int fd, sockaddr* addr, socklen_t* len);
int GetPeerName(int fd, sockaddr* addr, socklen_t* len);
// Bytes waiting to be read (FIONREAD).
int Available(int fd);
// Readiness of virtual sockets (Enabled() only): fills revents like poll() with no wait and
// returns how many have events.
int Poll(PollFd* fds, size_t count);
struct Ready
{
  bool read = false, write = false, error = false;
};
Ready Readiness(int fd);

// Name lookups. GetHostByName waits (up to about 15 s) for the Plaza's answer; the hostent
// stays valid until the next call on this thread.
hostent* GetHostByName(const char* name);
int GetAddrInfo(const char* node, const char* service, const addrinfo* hints, addrinfo** result);
void FreeAddrInfo(addrinfo* result);

// mbedtls BIO callbacks for SSL over a socket (virtual or not): the ctx is a pointer to the fd.
int MbedSend(void* ctx, const unsigned char* buf, size_t len);
int MbedRecv(void* ctx, unsigned char* buf, size_t len);
}  // namespace IOS::HLE::TamaribaNet
