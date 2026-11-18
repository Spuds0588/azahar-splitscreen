// Shared-memory frame ring: the split-screen producer side of the single-window
// mission. An instance writes each presented frame into slot N of a POSIX shm
// object; the viewer reads the newest complete slot. See
// `plans/single-window-shm.md`.
//
// Layout (little-endian, 64-bit fields are 8-byte aligned):
//   0   u32 magic      'A''Z''S''H'
//   4   u32 version    1
//   8   u32 width      frame width in pixels
//   12  u32 height     frame height in pixels
//   16  u32 stride     bytes per row
//   20  u32 format     0 = BGRA8, bottom-up (what the GL renderer reads back)
//   24  u32 slot_count 3
//   28  u32 slot_bytes width*height*4
//   32  u64 seq        incremented after a slot is complete
//   40  u64 request    viewer's pull counter (unused by the producer for now)
//   48  u64 reserved
//   56  u64 reserved
//   64  slots[slot_count]

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

class ShmFrameRing {
public:
    struct Header;

    ShmFrameRing() = default;
    ~ShmFrameRing();

    ShmFrameRing(const ShmFrameRing&) = delete;
    ShmFrameRing& operator=(const ShmFrameRing&) = delete;

    /// Create (and own) `/dev/shm/<name>`.
    bool Open(const std::string& name);
    void Close();
    bool IsOpen() const {
        return base_ != nullptr;
    }
    const std::string& Name() const {
        return name_;
    }

    /// Buffer to write the next frame into, or nullptr while a frame is in
    /// flight or the ring could not be (re)created for this size.
    std::uint8_t* BeginFrame(std::uint32_t width, std::uint32_t height);

    /// Publish the frame written by the matching EndFrame(). Safe to call from
    /// the renderer thread while BeginFrame() runs on another thread: at most
    /// one frame is ever in flight.
    void EndFrame();

private:
    bool Resize(std::uint32_t width, std::uint32_t height);
    Header* header() const {
        return reinterpret_cast<Header*>(base_);
    }

#ifdef _WIN32
    /// A named file mapping has no descriptor to reopen by name, so the handle
    /// is kept beside the view it owns.
    void* mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
    std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    std::string name_;
    std::atomic<bool> in_flight_{false};
    std::uint64_t published_ = 0;
};
