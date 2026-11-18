#include "citra_qt/input_socket.h"

#include "common/logging/log.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#ifdef _WIN32
namespace {
bool ensure_winsock() {
    static const bool started = [] {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        return true;
    }();
    return started;
}
} // namespace
#endif

InputSocket::~InputSocket() {
    Close();
}

bool InputSocket::Open(const std::string& path) {
    Close();
#ifdef _WIN32
    // Windows' unix-domain sockets are stream-only, so a seat listens on a
    // loopback datagram port instead: same 12-byte messages, same
    // fire-and-forget delivery, and non-blocking for free.
    const auto colon = path.find(':');
    if (colon == std::string::npos) {
        LOG_ERROR(Frontend, "input socket: expected host:port, got \"{}\"", path);
        return false;
    }
    ensure_winsock();
    socket_ = static_cast<std::uintptr_t>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (socket_ == kInvalidSocket) {
        LOG_ERROR(Frontend, "input socket: socket() failed: {}", WSAGetLastError());
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(std::atoi(path.c_str() + colon + 1)));
    if (::bind(reinterpret_cast<SOCKET>(socket_), reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
        LOG_ERROR(Frontend, "input socket: bind({}) failed: {}", path, WSAGetLastError());
        Close();
        return false;
    }
    u_long nonblocking = 1;
    ioctlsocket(reinterpret_cast<SOCKET>(socket_), FIONBIO, &nonblocking);
    path_ = path;
    LOG_INFO(Frontend, "input socket listening on {}", path);
    return true;
#else
    if (sizeof(sockaddr_un::sun_path) <= path.size()) {
        LOG_ERROR(Frontend, "input socket path is too long: {}", path);
        return false;
    }
    // SOCK_CLOEXEC/SOCK_NONBLOCK are Linux-only socket() flags; fcntl is the
    // portable way to get the same guarantees, and macOS needs it.
    fd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        LOG_ERROR(Frontend, "input socket: socket() failed: {}", std::strerror(errno));
        return false;
    }
    ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size());
    // A crashed session leaves its socket file behind; bind() would then fail
    // with EADDRINUSE, so take the path over.
    ::unlink(path.c_str());
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        LOG_ERROR(Frontend, "input socket: bind({}) failed: {}", path, std::strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    path_ = path;
    LOG_INFO(Frontend, "input socket listening on {}", path);
    return true;
#endif
}

void InputSocket::Close() {
#ifdef _WIN32
    if (socket_ != kInvalidSocket) {
        closesocket(reinterpret_cast<SOCKET>(socket_));
        socket_ = kInvalidSocket;
    }
#else
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!path_.empty()) {
        ::unlink(path_.c_str());
    }
#endif
    path_.clear();
}

std::size_t InputSocket::Drain(InputMessage* out, std::size_t capacity) {
    if (!IsOpen()) {
        return 0;
    }
    std::size_t count = 0;
#ifdef _WIN32
    while (count < capacity) {
        InputMessage message{};
        const int received =
            recvfrom(reinterpret_cast<SOCKET>(socket_), reinterpret_cast<char*>(&message),
                     sizeof(message), 0, nullptr, nullptr);
        if (received != static_cast<int>(sizeof(message))) {
            break; // nothing left, or a short/foreign datagram
        }
        out[count++] = message;
    }
    // Drop anything beyond what the caller can apply this tick rather than
    // letting a burst build up latency.
    char scratch[sizeof(InputMessage) * 16];
    while (recvfrom(reinterpret_cast<SOCKET>(socket_), scratch, sizeof(scratch), 0, nullptr,
                    nullptr) > 0) {
    }
#else
    while (count < capacity) {
        InputMessage message{};
        const ssize_t received = ::recv(fd_, &message, sizeof(message), MSG_DONTWAIT);
        if (received < static_cast<ssize_t>(sizeof(message))) {
            break; // nothing left, or a short/foreign datagram
        }
        out[count++] = message;
    }
    // Drop anything beyond what the caller can apply this tick rather than
    // letting a burst build up latency.
    char scratch[sizeof(InputMessage) * 16];
    while (::recv(fd_, scratch, sizeof(scratch), MSG_DONTWAIT) > 0) {
    }
#endif
    return count;
}
