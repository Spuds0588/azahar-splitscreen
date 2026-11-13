// Input socket: the split-screen consumer side of input for a headless seat.
//
// A seat that draws into shared memory has no window of its own to receive
// events, so the viewer forwards keys and touch as 12-byte datagrams to a unix
// datagram socket this instance owns. See `plans/single-window-shm.md`.
//
// Message layout (little-endian):
//   u8  type   1 = key down, 2 = key up, 3/4/5 = touch down/move/up
//   u8  pad[3]
//   u32 key    Qt::Key for key events, unused for touch
//   u16 x      touch position in console screen pixels
//   u16 y

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct InputMessage {
    std::uint8_t type;
    std::uint8_t pad[3];
    std::uint32_t key;
    std::uint16_t x;
    std::uint16_t y;
};

static_assert(sizeof(InputMessage) == 12, "input message must stay 12 bytes");

class InputSocket {
public:
    InputSocket() = default;
    ~InputSocket();

    InputSocket(const InputSocket&) = delete;
    InputSocket& operator=(const InputSocket&) = delete;

    /// Bind `<path>`, replacing a stale socket left by a crashed session.
    bool Open(const std::string& path);
    void Close();

    /// True once the socket is bound.
    bool IsOpen() const {
        return fd_ >= 0;
    }
    int Fd() const {
        return fd_;
    }

    /// Copy pending datagrams into `out`; returns how many were read.
    std::size_t Drain(InputMessage* out, std::size_t capacity);

private:
    int fd_ = -1;
    std::string path_;
};
