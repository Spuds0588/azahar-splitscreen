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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QTimer>

#include "citra_qt/bootmanager.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "input_common/main.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

namespace {
/// Set from a signal handler, polled on the GUI thread: quitting from a handler
/// is not safe, but writing a flag is.
volatile std::sig_atomic_t g_stop_requested = 0;

void OnStopSignal(int) {
    g_stop_requested = 1;
}

/// Reads the newest complete frame out of a ring and returns it top-down.
QImage ReadRingImage(const std::string& ring) {
    const int fd = ::open(("/dev/shm/" + ring).c_str(), O_RDONLY);
    if (fd < 0) {
        return {};
    }
    QImage image;
    struct stat info {};
    if (fstat(fd, &info) == 0 && static_cast<std::size_t>(info.st_size) > 64) {
        auto* base = static_cast<std::uint8_t*>(
            mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0));
        if (base != MAP_FAILED) {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t stride = 0;
            std::uint32_t slot_count = 0;
            std::uint64_t seq = 0;
            std::memcpy(&width, base + 8, 4);
            std::memcpy(&height, base + 12, 4);
            std::memcpy(&stride, base + 16, 4);
            std::memcpy(&slot_count, base + 24, 4);
            std::memcpy(&seq, base + 32, 8);
            if (slot_count > 0 && seq > 0 && width > 0 && height > 0) {
                const std::size_t slot_bytes = static_cast<std::size_t>(height) * stride;
                const std::size_t offset = 64 + ((seq - 1) % slot_count) * slot_bytes;
                if (offset + slot_bytes <= static_cast<std::size_t>(info.st_size)) {
                    image = QImage(base + offset, static_cast<int>(width), static_cast<int>(height),
                                   static_cast<int>(stride), QImage::Format_RGBA8888)
                                .mirrored(false, true)
                                .copy();
                }
            }
            munmap(base, static_cast<std::size_t>(info.st_size));
        }
    }
    ::close(fd);
    return image;
}

std::uint32_t HashFrame(const std::uint8_t* pixels, std::size_t bytes) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < bytes; i += 131) {
        hash = (hash ^ pixels[i]) * 16777619u;
    }
    return hash;
}

std::uint64_t RingSequence(const std::string& ring) {
    const int fd = ::open(("/dev/shm/" + ring).c_str(), O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    std::uint64_t seq = 0;
    if (pread(fd, &seq, sizeof(seq), 32) != sizeof(seq)) {
        seq = 0;
    }
    ::close(fd);
    return seq;
}

std::uint32_t RingHash(const std::string& ring) {
    const int fd = ::open(("/dev/shm/" + ring).c_str(), O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    std::uint32_t hash = 0;
    struct stat info {};
    if (fstat(fd, &info) == 0 && static_cast<std::size_t>(info.st_size) > 64) {
        auto* base = static_cast<std::uint8_t*>(
            mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0));
        if (base != MAP_FAILED) {
            std::uint32_t height = 0;
            std::uint32_t stride = 0;
            std::uint32_t slot_count = 0;
            std::uint64_t seq = 0;
            std::memcpy(&height, base + 12, 4);
            std::memcpy(&stride, base + 16, 4);
            std::memcpy(&slot_count, base + 24, 4);
            std::memcpy(&seq, base + 32, 8);
            if (slot_count > 0 && seq > 0) {
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

void SendInput(const std::string& socket_path, std::uint8_t type, std::uint32_t key,
               std::uint16_t x, std::uint16_t y) {
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path.c_str());
    std::uint8_t message[12] = {type, 0, 0, 0};
    std::memcpy(message + 4, &key, 4);
    std::memcpy(message + 8, &x, 2);
    std::memcpy(message + 10, &y, 2);
    ::sendto(fd, message, sizeof(message), 0, reinterpret_cast<sockaddr*>(&address),
             sizeof(address));
    ::close(fd);
}
} // namespace

InProcSeats::InProcSeats(int count, std::string rom, std::string ring_prefix,
                         std::string socket_prefix)
    : rom_{std::move(rom)}, ring_prefix_{std::move(ring_prefix)},
      socket_prefix_{std::move(socket_prefix)} {
    setWindowTitle(QStringLiteral("Azahar in-process seats"));
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    for (int i = 0; i < count; ++i) {
        auto seat = std::make_unique<Seat>();
        seat->system = std::make_unique<Core::System>();
        seat->ring = ring_prefix_ + std::to_string(i);
        seat->socket = socket_prefix_ + std::to_string(i) + ".sock";
        // Each seat renders into its own context and publishes its own ring; no
        // present thread is shared and no seat needs a visible window, so the
        // frames cannot starve each other.
        seat->window = new GRenderWindow(this, nullptr, *seat->system, false);
        seat->window->setAttribute(Qt::WA_DontShowOnScreen, true);
        seat->window->hide();
        seats_.push_back(std::move(seat));
    }
    pressed_seat_ = seats_.size();
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

    // Booting every seat in the same tick makes the guest apps race each other in
    // the shared user dir and one of them exits immediately (status 16,
    // ShutdownRequested). The four-process orchestrator staggers its spawns for
    // the same reason; do the same here.
    booting_ = 0;
    boot_timer_ = std::make_unique<QTimer>(this);
    connect(boot_timer_.get(), &QTimer::timeout, this, [this] {
        if (booting_ >= seats_.size()) {
            boot_timer_->stop();
            return;
        }
        BootSeat(*seats_[booting_], static_cast<int>(booting_));
        ++booting_;
    });
    boot_timer_->start(8000);

    frame_timer_ = std::make_unique<QTimer>(this);
    connect(frame_timer_.get(), &QTimer::timeout, this, [this] { update(); });
    frame_timer_->start(16);

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

    // A config would normally fill the input profile; without it HID creates no
    // devices at all, so touch and keys never reach the guest. Each seat names its
    // own window's touch factory so a touch stays on the seat it was sent to.
    auto& profile = Settings::values.current_input_profile;
    seat.system->SetTouchDeviceOverride("engine:" + seat.window->GetTouchDeviceName());
    profile.touch_device = "engine:emu_window";
    if (profile.buttons[Settings::NativeButton::Start].empty()) {
        profile.buttons[Settings::NativeButton::Start] = "engine:keyboard,code:77";
    }
    if (profile.buttons[Settings::NativeButton::A].empty()) {
        profile.buttons[Settings::NativeButton::A] = "engine:keyboard,code:65";
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
    // The first seat to boot writes the shared system settings and the guest then
    // asks APT for a restart, which the core reports as ShutdownRequested while it
    // is still in initial setup. Booting that seat again is what a fresh process
    // would do, so do it once rather than losing the seat.
    connect(seat.emu_thread.get(), &EmuThread::ErrorThrown, this,
            [this, index](Core::System::ResultStatus status, std::string details) {
                std::fprintf(stderr, "multiseat: seat%d core stopped, status=%d (%s)\n", index,
                             static_cast<int>(status), details.c_str());
                std::fflush(stderr);
            });
    seat.emu_thread->start();
    seat.emu_thread->SetRunning(true);
    seat.booted = true;
    std::fprintf(stderr, "multiseat: seat%d booted (ring=%s socket=%s)\n", index,
                 seat.ring.c_str(), seat.socket.c_str());
    return true;
}

void InProcSeats::paintEvent(QPaintEvent*) {
    QPainter painter{this};
    painter.fillRect(rect(), Qt::black);
    if (seats_.empty()) {
        return;
    }
    const int columns = seats_.size() > 1 ? 2 : 1;
    const int rows = (static_cast<int>(seats_.size()) + columns - 1) / columns;
    const int tile_width = width() / columns;
    const int tile_height = height() / rows;
    for (std::size_t i = 0; i < seats_.size(); ++i) {
        const int column = static_cast<int>(i) % columns;
        const int row = static_cast<int>(i) / columns;
        const QRect tile{column * tile_width, row * tile_height, tile_width, tile_height};
        const QImage frame = ReadRingImage(seats_[i]->ring);
        if (frame.isNull()) {
            continue;
        }
        const QSize scaled = frame.size().scaled(tile.size(), Qt::KeepAspectRatio);
        painter.drawImage(QPoint{tile.x() + (tile.width() - scaled.width()) / 2,
                                 tile.y() + (tile.height() - scaled.height()) / 2},
                          frame.scaled(scaled, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }
}

void InProcSeats::mousePressEvent(QMouseEvent* event) {
    if (seats_.empty()) {
        return;
    }
    const int columns = seats_.size() > 1 ? 2 : 1;
    const int rows = (static_cast<int>(seats_.size()) + columns - 1) / columns;
    const int column = std::clamp(static_cast<int>(event->position().x()) / (width() / columns), 0,
                                  columns - 1);
    const int row = std::clamp(static_cast<int>(event->position().y()) / (height() / rows), 0,
                               rows - 1);
    const std::size_t index = static_cast<std::size_t>(row * columns + column);
    if (index >= seats_.size()) {
        return;
    }
    // Same datagram the viewer sends, so the seat's socket path is what carries it.
    const QRect tile{column * (width() / columns), row * (height() / rows), width() / columns,
                     height() / rows};
    const QSize frame_size = ReadRingImage(seats_[index]->ring).size();
    if (frame_size.isEmpty()) {
        return;
    }
    const QSize scaled = frame_size.scaled(tile.size(), Qt::KeepAspectRatio);
    const QPoint origin{tile.x() + (tile.width() - scaled.width()) / 2,
                        tile.y() + (tile.height() - scaled.height()) / 2};
    const QPointF local = event->position() - QPointF{origin};
    const int x = static_cast<int>(local.x() * frame_size.width() / std::max(1, scaled.width()));
    const int y = static_cast<int>(local.y() * frame_size.height() / std::max(1, scaled.height()));
    if (x < 0 || y < 0 || x >= frame_size.width() || y >= frame_size.height()) {
        return;
    }
    SendInput(seats_[index]->socket, 3, 0, static_cast<std::uint16_t>(x),
              static_cast<std::uint16_t>(y));
    pressed_seat_ = index;
}

void InProcSeats::mouseReleaseEvent(QMouseEvent*) {
    if (pressed_seat_ < seats_.size()) {
        SendInput(seats_[pressed_seat_]->socket, 5, 0, 0, 0);
        pressed_seat_ = seats_.size();
    }
}

void InProcSeats::LogStats() {
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    std::string line = "multiseat: t=" + std::to_string(static_cast<int>(seconds)) + "s";
    int live = 0;
    for (std::size_t i = 0; i < seats_.size(); ++i) {
        Seat& seat = *seats_[i];
        const std::uint64_t seq = RingSequence(seat.ring);
        const std::uint32_t hash = RingHash(seat.ring);
        const double fps = static_cast<double>(seq - seat.last_seq);
        seat.last_seq = seq;
        seat.hash = hash;
        if (seq > 0) {
            ++live;
        }
        char text[32];
        std::snprintf(text, sizeof(text), "%08x", hash);
        if (!seat.system->IsPoweredOn() && seat.booted && seat.failure.empty()) {
            seat.failure = seat.system->GetStatusDetails();
            std::fprintf(stderr, "multiseat: seat%zu stopped (%s)\n", i, seat.failure.c_str());
        }
        line += " seat" + std::to_string(i) +
                " fps=" + std::to_string(static_cast<int>(fps + 0.5)) +
                " seq=" + std::to_string(seq) + " hash=" + text;
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
    if (frame_timer_) {
        frame_timer_->stop();
    }
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
            seat->emu_thread->RequestStop();
            // The emulation thread sleeps in the frame limiter between slices, so
            // it only notices the stop when it is woken.
            for (int attempt = 0; attempt < 40 && seat->emu_thread->isRunning(); ++attempt) {
                seat->system->NotifyPendingWork();
                seat->emu_thread->wait(50);
            }
            if (seat->emu_thread->isRunning()) {
                // A thread that will not leave must not outlive the object whose
                // condition variables it waits on; that is the shutdown hang.
                std::fprintf(stderr, "multiseat: seat emulation thread did not stop, terminating\n");
                seat->emu_thread->terminate();
                seat->emu_thread->wait(2000);
            }
            seat->emu_thread.reset();
        }
        if (seat->window) {
            seat->window->StopSharedFrames();
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
