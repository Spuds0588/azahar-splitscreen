// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <QWidget>

class EmuThread;
class GRenderWindow;
class QTimer;

namespace Core {
class System;
}

/**
 * The in-process seats spike: N Core::System instances and N emulation threads
 * inside one process, composited into one window.
 *
 * Each seat keeps its own render context, shared-memory ring and input socket —
 * the same producer path a seat process uses — and this widget is the single
 * window that draws all of their rings. There is no shared present thread: each
 * seat's ring is refreshed by its own timer, so one seat cannot starve another.
 * `--inproc-seats N <rom>` starts it; see plans/in-process-seats.md.
 */
class InProcSeats : public QWidget {
public:
    InProcSeats(int count, std::string rom, std::string ring_prefix, std::string socket_prefix);
    ~InProcSeats() override;

    /// Boots every seat. Call after the window is shown, so each seat's render
    /// target has a surface to bind its context to.
    bool Start();

    /// Stops every seat and removes its threads, ring and socket.
    void Shutdown();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    struct Seat {
        std::unique_ptr<Core::System> system;
        GRenderWindow* window = nullptr;
        std::unique_ptr<EmuThread> emu_thread;
        std::string ring;
        std::string socket;
        std::uint64_t last_seq = 0;
        std::uint32_t hash = 0;
        bool booted = false;
        std::string failure;
    };

    bool BootSeat(Seat& seat, int index);
    void LogStats();
    void RequestStop();

    std::vector<std::unique_ptr<Seat>> seats_;
    std::string rom_;
    std::string ring_prefix_;
    std::string socket_prefix_;
    std::unique_ptr<QTimer> boot_timer_;
    std::size_t booting_ = 0;
    std::unique_ptr<QTimer> frame_timer_;
    std::unique_ptr<QTimer> stats_timer_;
    std::unique_ptr<QTimer> stop_poll_timer_;
    std::size_t pressed_seat_ = 0;
    std::chrono::steady_clock::time_point started_{};
    bool shut_down_ = false;
};
