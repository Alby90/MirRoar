// MirRoar server - minimal BSD-socket portability layer (Winsock / POSIX).
#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline void closeSocket(socket_t s) { closesocket(s); }
inline void shutdownSocket(socket_t s) { shutdown(s, SD_BOTH); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline void closeSocket(socket_t s) { close(s); }
inline void shutdownSocket(socket_t s) { shutdown(s, SHUT_RDWR); }
#endif
