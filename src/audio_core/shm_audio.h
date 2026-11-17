// Shared-memory audio ring: the seat-side producer for the split-screen
// mission's single audio stream. A headless seat writes what it would have
// played into slot-less ring of interleaved stereo PCM16; the libretro core
// (or the one window, later) reads it and owns the only device.
//
// Layout (little-endian, 64-byte header so it mirrors the frame ring):
//   0   u32 magic            'A''Z''S''A'
//   4   u32 version          1
//   8   u32 sample_rate      the rate the samples are written at
//   12  u32 channels         2 (interleaved stereo)
//   16  u32 capacity_frames  frames the ring holds
//   20  u32 reserved
//   24  u64 write_frames     monotonic count of frames published
//   32  u64 reserved[4]
//   64  s16 samples[capacity_frames * 2]
//
// The producer never blocks: a reader that falls behind simply loses its
// oldest frames, the same trade the frame ring makes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "common/common_types.h"

namespace AudioCore {

class ShmAudioRing {
public:
    struct Header;

    ShmAudioRing() = default;
    ~ShmAudioRing();

    ShmAudioRing(const ShmAudioRing&) = delete;
    ShmAudioRing& operator=(const ShmAudioRing&) = delete;

    /// Create (and own) `/dev/shm/<name>`, sized for `sample_rate`.
    bool Open(const std::string& name, u32 sample_rate);
    void Close();
    bool IsOpen() const {
        return base_ != nullptr;
    }
    const std::string& Name() const {
        return name_;
    }

    /// Append interleaved stereo frames. Called from the audio path, so it does
    /// no allocation and no locking.
    void Push(const s16* interleaved_stereo, std::size_t frames);

private:
    Header* header() const;

    int fd_ = -1;
    std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    std::string name_;
};

/// Route the emulated audio into `ring` and keep the audio device closed.
/// A null ring restores the configured sink.
void SetShmAudioRing(std::shared_ptr<ShmAudioRing> ring);
std::shared_ptr<ShmAudioRing> GetShmAudioRing();

} // namespace AudioCore
