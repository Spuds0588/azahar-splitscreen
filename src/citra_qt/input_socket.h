// Input socket: the split-screen consumer side of input for a headless seat.
//
// A seat that draws into shared memory has no window of its own to receive
// events, so the viewer forwards keys, touch and the circle pad as 12-byte
// datagrams to a unix datagram socket this instance owns. See
// `plans/single-window-shm.md`.
//
// Message layout (little-endian):
//   u8  type   1 = key down, 2 = key up, 3/4/5 = touch down/move/up,
//              6 = circle pad
//   u8  pad[3]
//   u32 key    Qt::Key for key events, unused for touch and the circle pad
//   u16 x      touch position in console screen pixels; for type 6, the circle
//              pad's x as the bits of a signed int16
//   u16 y      the same for the circle pad's y, which points up
//
// The circle pad travels as the stick's own position, not as arrow keys, so a
// half-deflected stick is a half-deflected circle pad rather than a full one.

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
