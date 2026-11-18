// Shared-memory audio ring producer. See shm_audio.h for the layout.

#include "audio_core/shm_audio.h"

#include <cstring>
#include <mutex>

#include "common/logging/log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace AudioCore {
namespace {
constexpr std::uint32_t kMagic = 0x41535A41; // 'AZSA'
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kCapacityFrames = 16384; // half a second at 32728 Hz
constexpr std::size_t kHeaderBytes = 64;

#if !defined(_WIN32) && !defined(__ANDROID__)
// shm_open takes a POSIX name, which must start with a slash on Linux and macOS.
std::string ShmName(const std::string& name) {
    return (!name.empty() && name.front() == '/') ? name : "/" + name;
}
#endif

std::mutex registry_mutex;
std::shared_ptr<ShmAudioRing> active_ring;
} // namespace

struct ShmAudioRing::Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t sample_rate;
    std::uint32_t channels;
    std::uint32_t capacity_frames;
    std::uint32_t reserved;
    std::uint64_t write_frames;
    std::uint64_t reserved2[4];
    // Samples follow at kHeaderBytes.
};

static_assert(sizeof(ShmAudioRing::Header) == kHeaderBytes,
              "shm audio header must stay 64 bytes to match the documented layout");

ShmAudioRing::~ShmAudioRing() {
    Close();
}

ShmAudioRing::Header* ShmAudioRing::header() const {
    return reinterpret_cast<Header*>(base_);
}

bool ShmAudioRing::Open(const std::string& name, u32 sample_rate) {
    Close();
    name_ = name;
    const std::size_t needed =
        kHeaderBytes + static_cast<std::size_t>(kCapacityFrames) * kChannels * sizeof(s16);
    void* mapped = nullptr;
#ifdef _WIN32
    // A named file mapping replaces /dev/shm. The ring is a fixed size, so it is
    // created at that size and never has to grow.
    mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(needed), ("Local\\" + name).c_str());
    if (mapping_ == nullptr) {
        LOG_ERROR(Audio, "shm audio ring \"{}\": CreateFileMapping failed ({})", name,
                  GetLastError());
        return false;
    }
    mapped = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (mapped == nullptr) {
        LOG_ERROR(Audio, "shm audio ring \"{}\": MapViewOfFile failed ({})", name, GetLastError());
        Close();
        return false;
    }
#elif defined(__ANDROID__)
    // Android's libc has no POSIX shm objects, so a seat there plays through its
    // configured sink instead of a ring.
    LOG_WARNING(Audio, "shm audio ring \"{}\": not available on Android", name);
    return false;
#else
    fd_ = shm_open(ShmName(name).c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
        LOG_ERROR(Audio, "shm audio ring \"{}\": shm_open failed", name);
        return false;
    }
    if (ftruncate(fd_, static_cast<off_t>(needed)) != 0) {
        LOG_ERROR(Audio, "shm audio ring \"{}\": ftruncate to {} failed", name, needed);
        Close();
        return false;
    }
    mapped = mmap(nullptr, needed, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
        LOG_ERROR(Audio, "shm audio ring \"{}\": mmap failed", name);
        Close();
        return false;
    }
#endif
    base_ = static_cast<std::uint8_t*>(mapped);
    size_ = needed;
    std::memset(base_, 0, kHeaderBytes);
    header()->magic = kMagic;
    header()->version = kVersion;
    header()->sample_rate = sample_rate;
    header()->channels = kChannels;
    header()->capacity_frames = kCapacityFrames;
    header()->write_frames = 0;
    LOG_INFO(Audio, "shm audio ring \"{}\" created: {} Hz, {} frames", name, sample_rate,
             kCapacityFrames);
    return true;
}

void ShmAudioRing::Close() {
#ifdef _WIN32
    if (base_ != nullptr) {
        UnmapViewOfFile(base_);
        base_ = nullptr;
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
#else
    if (base_ != nullptr) {
        munmap(base_, size_);
        base_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
#ifndef __ANDROID__
        shm_unlink(ShmName(name_).c_str()); // the reader keeps its own mapping alive
#endif
    }
#endif
    size_ = 0;
}

void ShmAudioRing::Push(const s16* interleaved_stereo, std::size_t frames) {
    if (base_ == nullptr || interleaved_stereo == nullptr || frames == 0) {
        return;
    }
    Header* head = header();
    const std::uint32_t capacity = head->capacity_frames;
    if (capacity == 0) {
        return;
    }
    auto* samples = reinterpret_cast<s16*>(base_ + kHeaderBytes);
    std::uint64_t write = __atomic_load_n(&head->write_frames, __ATOMIC_RELAXED);
    // More than a ring of new data at once: the reader is gone or stalled, so
    // keep only what would survive anyway.
    if (frames > capacity) {
        interleaved_stereo += (frames - capacity) * kChannels;
        write += frames - capacity;
        frames = capacity;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t at = static_cast<std::size_t>((write + i) % capacity) * kChannels;
        samples[at + 0] = interleaved_stereo[i * kChannels + 0];
        samples[at + 1] = interleaved_stereo[i * kChannels + 1];
    }
    __atomic_store_n(&head->write_frames, write + frames, __ATOMIC_RELEASE);
}

void SetShmAudioRing(std::shared_ptr<ShmAudioRing> ring) {
    std::scoped_lock lock{registry_mutex};
    active_ring = std::move(ring);
}

std::shared_ptr<ShmAudioRing> GetShmAudioRing() {
    std::scoped_lock lock{registry_mutex};
    return active_ring;
}

} // namespace AudioCore
