#include "citra_qt/shm_frame.h"

#include "common/logging/log.h"

#include <cstring>
#include <string>

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

namespace {
constexpr std::uint32_t kMagic = 0x48535A41; // 'AZSH'
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kSlotCount = 3;
constexpr std::size_t kHeaderBytes = 64;
constexpr std::uint32_t kFormatBgraBottomUp = 0;
#ifdef _WIN32
// Windows cannot grow a mapping, so one is created at the largest frame it will
// ever carry (2048x2048 per slot) and only pages actually written commit.
constexpr std::size_t kMaxSlotBytes = 2048u * 2048u * 4u;
constexpr std::size_t kMaxBytes = kHeaderBytes + kMaxSlotBytes * kSlotCount;
#else
// shm_open takes a POSIX name, which must start with a slash on both Linux and
// macOS. Session logs and the viewer keep using the plain name.
std::string ShmName(const std::string& name) {
    return (!name.empty() && name.front() == '/') ? name : "/" + name;
}
#endif
} // namespace

struct ShmFrameRing::Header {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t format;
    std::uint32_t slot_count;
    std::uint32_t slot_bytes;
    std::uint64_t seq;
    std::uint64_t request;
    std::uint64_t reserved[2];
    // Slots follow at kHeaderBytes.
};

static_assert(sizeof(ShmFrameRing::Header) == kHeaderBytes,
              "shm header must stay 64 bytes to match the documented layout");

ShmFrameRing::~ShmFrameRing() {
    Close();
}

bool ShmFrameRing::Open(const std::string& name) {
    Close();
    name_ = name;
#ifdef _WIN32
    // A named file mapping replaces /dev/shm: no path, no stale file to clear,
    // and it disappears with its last handle.
    mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(kMaxBytes), ("Local\\" + name).c_str());
    if (mapping_ == nullptr) {
        LOG_ERROR(Frontend, "shm frame ring \"{}\": CreateFileMapping failed ({})", name,
                  GetLastError());
        return false;
    }
    void* mapped = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (mapped == nullptr) {
        LOG_ERROR(Frontend, "shm frame ring \"{}\": MapViewOfFile failed ({})", name,
                  GetLastError());
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    base_ = static_cast<std::uint8_t*>(mapped);
    size_ = kMaxBytes;
    std::memset(base_, 0, kHeaderBytes);
    header()->magic = kMagic;
    header()->version = kVersion;
    header()->slot_count = kSlotCount;
    header()->format = kFormatBgraBottomUp;
    LOG_INFO(Frontend, "shm frame ring \"{}\" created", name);
    return true;
#else
    fd_ = shm_open(ShmName(name).c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
        LOG_ERROR(Frontend, "shm frame ring \"{}\": shm_open failed", name);
        return false;
    }
    // Start with a header-only mapping; the real size is known once the first
    // frame arrives and stays fixed for the session.
    if (ftruncate(fd_, static_cast<off_t>(kHeaderBytes)) != 0) {
        LOG_ERROR(Frontend, "shm frame ring \"{}\": ftruncate failed", name);
        Close();
        return false;
    }
    void* mapped = mmap(nullptr, kHeaderBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
        LOG_ERROR(Frontend, "shm frame ring \"{}\": mmap failed", name);
        Close();
        return false;
    }
    base_ = static_cast<std::uint8_t*>(mapped);
    size_ = kHeaderBytes;
    std::memset(base_, 0, kHeaderBytes);
    header()->magic = kMagic;
    header()->version = kVersion;
    header()->slot_count = kSlotCount;
    header()->format = kFormatBgraBottomUp;
    LOG_INFO(Frontend, "shm frame ring \"{}\" created", name);
    return true;
#endif
}

void ShmFrameRing::Close() {
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
        shm_unlink(ShmName(name_).c_str()); // the viewer keeps its own mapping alive
    }
#endif
    size_ = 0;
    in_flight_.store(false, std::memory_order_release);
    published_ = 0;
}

bool ShmFrameRing::Resize(std::uint32_t width, std::uint32_t height) {
    const std::uint32_t slot_bytes = width * height * 4;
    const std::size_t needed = kHeaderBytes + static_cast<std::size_t>(slot_bytes) * kSlotCount;

    std::uint64_t seq = base_ ? __atomic_load_n(&header()->seq, __ATOMIC_ACQUIRE) : 0;
#ifdef _WIN32
    if (needed > size_) {
        LOG_ERROR(Frontend, "shm frame ring \"{}\": {}x{} needs {} bytes, the mapping holds {}",
                  name_, width, height, needed, size_);
        return false;
    }
    // Already the largest it will ever be: only the header has to change.
#else
    if (base_ != nullptr) {
        munmap(base_, size_);
        base_ = nullptr;
    }
    if (ftruncate(fd_, static_cast<off_t>(needed)) != 0) {
        LOG_ERROR(Frontend, "shm frame ring \"{}\": ftruncate to {} failed", name_, needed);
        return false;
    }
    void* mapped = mmap(nullptr, needed, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
        LOG_ERROR(Frontend, "shm frame ring \"{}\": remap to {} failed", name_, needed);
        return false;
    }
    base_ = static_cast<std::uint8_t*>(mapped);
    size_ = needed;
#endif

    header()->magic = kMagic;
    header()->version = kVersion;
    header()->width = width;
    header()->height = height;
    header()->stride = width * 4;
    header()->format = kFormatBgraBottomUp;
    header()->slot_count = kSlotCount;
    header()->slot_bytes = slot_bytes;
    __atomic_store_n(&header()->seq, seq, __ATOMIC_RELEASE);
    LOG_INFO(Frontend, "shm frame ring \"{}\": {}x{} x{} slots ({} bytes)", name_, width, height,
             kSlotCount, needed);
    return true;
}

std::uint8_t* ShmFrameRing::BeginFrame(std::uint32_t width, std::uint32_t height) {
    if (!IsOpen() || width == 0 || height == 0) {
        return nullptr;
    }
    bool expected = false;
    if (!in_flight_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return nullptr; // a frame is still being read back
    }
    if (!base_ || header()->width != width || header()->height != height) {
        if (!Resize(width, height)) {
            return nullptr;
        }
    }
    published_ = __atomic_load_n(&header()->seq, __ATOMIC_ACQUIRE);
    const auto slot = static_cast<std::uint32_t>((published_ + 1) % kSlotCount);
    return base_ + kHeaderBytes + static_cast<std::size_t>(slot) * header()->slot_bytes;
}

void ShmFrameRing::EndFrame() {
    if (!IsOpen()) {
        return;
    }
    bool expected = true;
    if (!in_flight_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return; // nothing in flight (already published or never requested)
    }
    __atomic_store_n(&header()->seq, published_ + 1, __ATOMIC_RELEASE);
}
