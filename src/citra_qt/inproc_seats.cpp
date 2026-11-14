// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include "citra_qt/inproc_seats.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QGridLayout>
#include <QTimer>

#include "citra_qt/bootmanager.h"
#include "core/core.h"
#include "input_common/main.h"
#include "core/frontend/applets/default_applets.h"

namespace {
/// Set from a signal handler, polled on the GUI thread: quitting from a handler
/// is not safe, but writing a flag is.
volatile std::sig_atomic_t g_stop_requested = 0;

void OnStopSignal(int) {
    g_stop_requested = 1;
}

std::uint32_t HashFrame(const std::uint8_t* pixels, std::size_t bytes) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < bytes; i += 131) {
        hash = (hash ^ pixels[i]) * 16777619u;
    }
    return hash;
}

double ProcessCpuSeconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
}

long ProcessRssMb() {
    std::FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return 0;
    }
    char line[256];
    long kb = 0;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::strncmp(line, "VmRSS:", 6) == 0) {
            kb = std::strtol(line + 6, nullptr, 10);
            break;
        }
    }
    std::fclose(status);
    return kb / 1024;
}
} // namespace

InProcSeats::InProcSeats(int count, std::string rom, std::string ring_prefix,
                         std::string socket_prefix)
    : rom_{std::move(rom)}, ring_prefix_{std::move(ring_prefix)},
      socket_prefix_{std::move(socket_prefix)} {
    setWindowTitle(QStringLiteral("Azahar in-process seats"));
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(1);

    const int columns = count > 1 ? 2 : 1;
    for (int i = 0; i < count; ++i) {
        auto seat = std::make_unique<Seat>();
        seat->system = std::make_unique<Core::System>();
        seat->ring = ring_prefix_ + std::to_string(i);
        seat->socket = socket_prefix_ + std::to_string(i) + ".sock";
        seat->window = new GRenderWindow(this, nullptr, *seat->system, false);
        grid->addWidget(seat->window, i / columns, i % columns);
        seats_.push_back(std::move(seat));
    }
    resize(1280, 720);
}

InProcSeats::~InProcSeats() {
    Shutdown();
}

bool InProcSeats::Start() {
    started_ = std::chrono::steady_clock::now();
    // The input stack (SDL, keyboard, motion, UDP) is process-wide, not per seat;
    // the frontend normally initialises it before any game boots.
    InputCommon::Init();
    std::signal(SIGTERM, OnStopSignal);
    std::signal(SIGINT, OnStopSignal);

    bool ok = true;
    for (std::size_t i = 0; i < seats_.size(); ++i) {
        ok = BootSeat(*seats_[i], static_cast<int>(i)) && ok;
    }
    if (!ok) {
        return false;
    }

    stats_timer_ = std::make_unique<QTimer>(this);
    connect(stats_timer_.get(), &QTimer::timeout, this, &InProcSeats::LogStats);
    stats_timer_->start(1000);

    stop_poll_timer_ = std::make_unique<QTimer>(this);
    connect(stop_poll_timer_.get(), &QTimer::timeout, this, [this] {
        if (g_stop_requested != 0) {
            RequestStop();
        }
    });
    stop_poll_timer_->start(100);
    return true;
}

bool InProcSeats::BootSeat(Seat& seat, int index) {
    // Core code on this thread must resolve to this seat until the boot ends.
    Core::System::SetCurrent(seat.system.get());
    Frontend::RegisterDefaultApplets(*seat.system);

    seat.window->SetShmFrame(seat.ring);
    seat.window->SetInputSocket(seat.socket);

    if (!seat.window->InitRenderTarget()) {
        std::fprintf(stderr, "multiseat: seat%d failed to create a render target\n", index);
        return false;
    }

    Core::System::ResultStatus result;
    {
        const auto scope = seat.window->Acquire();
        result = seat.system->Load(*seat.window, rom_);
    }
    if (result != Core::System::ResultStatus::Success) {
        std::fprintf(stderr, "multiseat: seat%d failed to load the ROM (status %d)\n", index,
                     static_cast<int>(result));
        return false;
    }

    // The frame limiter starts in frame-advance mode (the frontend normally
    // releases it once the loading screen closes); a seat that never gets this
    // waits forever before its first frame.
    seat.system->frame_limiter.SetFrameAdvancing(false);

    seat.emu_thread = std::make_unique<EmuThread>(*seat.system, *seat.window);
    seat.window->OnEmulationStarting(seat.emu_thread.get());
    seat.emu_thread->start();
    seat.emu_thread->SetRunning(true);
    seat.booted = true;
    std::fprintf(stderr, "multiseat: seat%d booted (ring=%s socket=%s)\n", index,
                 seat.ring.c_str(), seat.socket.c_str());
    return true;
}

void InProcSeats::LogStats() {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    std::string line = "multiseat: t=" + std::to_string(static_cast<int>(seconds)) + "s";
    int live = 0;
    for (std::size_t i = 0; i < seats_.size(); ++i) {
        Seat& seat = *seats_[i];
        std::uint64_t seq = 0;
        std::uint32_t hash = 0;
        if (const int fd = ::open(("/dev/shm/" + seat.ring).c_str(), O_RDONLY); fd >= 0) {
            struct stat info {};
            if (fstat(fd, &info) == 0 && static_cast<std::size_t>(info.st_size) > 64) {
                auto* base = static_cast<std::uint8_t*>(
                    mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_SHARED,
                         fd, 0));
                if (base != MAP_FAILED) {
                    std::uint32_t width = 0;
                    std::uint32_t height = 0;
                    std::uint32_t stride = 0;
                    std::uint32_t slot_count = 0;
                    std::memcpy(&width, base + 8, 4);
                    std::memcpy(&height, base + 12, 4);
                    std::memcpy(&stride, base + 16, 4);
                    std::memcpy(&slot_count, base + 24, 4);
                    std::memcpy(&seq, base + 32, 8);
                    if (slot_count > 0 && seq > 0 && width > 0 && height > 0) {
                        const std::size_t slot_bytes = static_cast<std::size_t>(height) * stride;
                        const std::size_t offset = 64 + ((seq - 1) % slot_count) * slot_bytes;
                        if (offset + slot_bytes <= static_cast<std::size_t>(info.st_size)) {
                            hash = HashFrame(base + offset, slot_bytes);
                        }
                    }
                    munmap(base, static_cast<std::size_t>(info.st_size));
                }
            }
            ::close(fd);
        }
        const double fps = static_cast<double>(seq - seat.last_seq);
        seat.last_seq = seq;
        seat.hash = hash;
        if (seq > 0) {
            ++live;
        }
        line += " seat" + std::to_string(i) + " fps=" + std::to_string(static_cast<int>(fps + 0.5)) +
                " seq=" + std::to_string(seq) + " hash=" + [&] {
                    char text[16];
                    std::snprintf(text, sizeof(text), "%08x", hash);
                    return std::string(text);
                }();
    }

    static double last_cpu = 0.0;
    static double last_seconds = 0.0;
    const double cpu = ProcessCpuSeconds();
    const double cores = seconds > last_seconds ? (cpu - last_cpu) / (seconds - last_seconds) : 0.0;
    last_cpu = cpu;
    last_seconds = seconds;
    char tail[96];
    std::snprintf(tail, sizeof(tail), " | live=%d/%zu cpu=%.0f%% rss=%ldMB", live, seats_.size(),
                  cores * 100.0, ProcessRssMb());
    line += tail;
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

void InProcSeats::RequestStop() {
    Shutdown();
    if (auto* app = QCoreApplication::instance(); app != nullptr) {
        app->quit();
    }
}

void InProcSeats::Shutdown() {
    if (shut_down_) {
        return;
    }
    shut_down_ = true;
    if (stats_timer_) {
        stats_timer_->stop();
    }
    if (stop_poll_timer_) {
        stop_poll_timer_->stop();
    }

    for (auto& seat : seats_) {
        if (seat->emu_thread) {
            seat->system->RequestShutdown();
            seat->emu_thread->SetRunning(false);
            seat->window->OnEmulationStopping();
            // The emulation thread sleeps in the frame limiter between slices, so
            // it only notices the stop when it is woken.
            for (int attempt = 0; attempt < 100 && seat->emu_thread->isRunning(); ++attempt) {
                seat->system->NotifyPendingWork();
                seat->emu_thread->wait(100);
            }
        }
        if (seat->system->IsPoweredOn()) {
            seat->system->Shutdown();
        }
        if (seat->window) {
            seat->window->ReleaseRenderTarget();
        }
        ::unlink(seat->socket.c_str());
        ::unlink(("/dev/shm/" + seat->ring).c_str());
    }
    InputCommon::Shutdown();
    std::fprintf(stderr, "multiseat: stopped %zu seats\n", seats_.size());
    std::fflush(stderr);
}
