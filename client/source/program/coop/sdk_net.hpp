#pragma once
/* Minimal nn::socket / nn::nifm declarations. Resolved at load time against the
   game's nnSdk module (exlaunch links our module dynamically against it).
   Signatures follow SuperMarioOdysseyOnline/include/nn/socket.h. */
#include "common.hpp"

/* nn::Result comes from exlaunch (typedef u32 Result; 0 == success). */

struct in_addr   { u32 data; };
struct sockaddr  { u8 len; u8 family; u16 port; in_addr address; u8 zero[8]; };
struct pollfd    { s32 fd; s16 events; s16 revents; };

namespace nn::socket {
    struct Config;
    /* Skyrim initialises the socket library itself (nnMain); we only hook that call and wait. */
    Result Initialize(const Config& config);
    s32 Socket(s32 domain, s32 type, s32 protocol);
    s32 Connect(s32 fd, const sockaddr* addr, u32 addrLen);
    s32 Send(s32 fd, const void* data, u64 len, s32 flags);
    s32 Recv(s32 fd, void* out, u64 len, s32 flags);
    s32 SetSockOpt(s32 fd, s32 level, s32 option, const void* value, u32 len);
    Result Close(s32 fd);
    u16 InetHtons(u16 v);
    s32 InetAton(const char* str, in_addr* out);
    u32 GetLastErrno();
    s32 SendTo(s32 fd, const void* data, u64 len, s32 flags, const sockaddr* to, u32 toLen);
    s32 RecvFrom(s32 fd, void* out, u64 len, s32 flags, sockaddr* from, u32* fromLen);
    s32 Bind(s32 fd, const sockaddr* addr, u32 addrLen);
    s32 Fcntl(s32 fd, s32 cmd, ...);
    s32 Poll(pollfd* fds, u64 count, s32 timeoutMs);
    s32 GetSockOpt(s32 fd, s32 level, s32 option, void* value, u32* len);
}

namespace nn::nifm {
    Result Initialize();
    void SubmitNetworkRequest();
    bool IsNetworkRequestOnHold();
    bool IsNetworkAvailable();
    Result GetCurrentIpConfigInfo(in_addr* ip, in_addr* mask, in_addr* gateway, in_addr* dns1, in_addr* dns2);
}

namespace coop::net {
    constexpr s32 AF_INET = 2, SOCK_STREAM = 1, SOCK_DGRAM = 2, IPPROTO_TCP = 6, IPPROTO_UDP = 17, TCP_NODELAY = 1;
    constexpr s32 MSG_DONTWAIT = 0x80;
    /* FreeBSD values (Horizon's bsd service) */
    constexpr s32 SOL_SOCKET = 0xFFFF, SO_BROADCAST = 0x20, SO_ERROR = 0x1007;
    constexpr s32 F_GETFL = 3, F_SETFL = 4, O_NONBLOCK = 4;
    constexpr s16 POLLOUT = 4;
}
