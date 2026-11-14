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
 * The in-process seats spike: N Core::System instances, N emulation threads and
 * N render contexts inside one process, tiled in one window.
 *
 * It exists to answer whether one process can hold four seats, so it deliberately
 * reuses the production seat path (GRenderWindow, the shared-memory ring and the
 * input socket) rather than inventing a second one. `--inproc-seats N <rom>`
 * starts it; see plans/in-process-seats.md.
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
    };

    bool BootSeat(Seat& seat, int index);
    void LogStats();
    void RequestStop();

    std::vector<std::unique_ptr<Seat>> seats_;
    std::string rom_;
    std::string ring_prefix_;
    std::string socket_prefix_;
    std::unique_ptr<QTimer> stats_timer_;
    std::unique_ptr<QTimer> stop_poll_timer_;
    std::chrono::steady_clock::time_point started_{};
    bool shut_down_ = false;
};
