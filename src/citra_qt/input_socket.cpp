#include "citra_qt/input_socket.h"

#include "common/logging/log.h"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

InputSocket::~InputSocket() {
    Close();
}

bool InputSocket::Open(const std::string& path) {
    Close();
    if (sizeof(sockaddr_un::sun_path) <= path.size()) {
        LOG_ERROR(Frontend, "input socket path is too long: {}", path);
        return false;
    }
    fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd_ < 0) {
        LOG_ERROR(Frontend, "input socket: socket() failed: {}", std::strerror(errno));
        return false;
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
}

void InputSocket::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!path_.empty()) {
        ::unlink(path_.c_str());
        path_.clear();
    }
}

std::size_t InputSocket::Drain(InputMessage* out, std::size_t capacity) {
    if (fd_ < 0) {
        return 0;
    }
    std::size_t count = 0;
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
    return count;
}
