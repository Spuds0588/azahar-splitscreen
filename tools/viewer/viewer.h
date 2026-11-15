// azahar-viewer — the single window of a split-screen session.
//
// One window, one tile per seat. Each seat's emulator process renders into a
// shared-memory frame ring; this reads the newest complete frame from each ring
// and draws it. It never blocks on a seat: a tile whose ring has not advanced
// simply keeps its previous frame.
//
// Input goes the other way: keys and touch clicks are forwarded as datagrams to
// the selected seat's socket, so the emulator instances stay headless.
//
// Painting is plain QPainter, not GL: this session's compositor throttles GL
// swaps to ~1 Hz (glxgears measures 1-2 fps), while the same tiles painted with
// QPainter run at the timer's rate. It is also what the in-process compositor
// (`--inproc-seats`) uses, so both one-window paths present the same way.

#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QPainter;

#include <vector>

/// One seat: its frame ring plus the socket its input goes to.
struct Seat {
    QString ring_name;
    QString socket_path;
    /// Marker file the orchestrator creates while it restarts this seat; the
    /// tile says so until the marker is gone.
    QString restart_path;
    bool restarting = false;

    // Frame ring, mapped read-only. Reopened until the producer sizes it.
    int fd = -1;
    quint8* base = nullptr;
    qsizetype size = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint32 stride = 0;
    quint32 slot_count = 0;
    quint32 slot_bytes = 0;
    quint64 last_seq = 0;
    quint64 frames = 0;
    bool missing_reported = false;

    QImage canvas; // newest frame, top-down BGRA (Format_ARGB32 on LE)
};

class Viewer : public QWidget {
    Q_OBJECT

public:
    /// `message` turns the window into a plain notice: no rings, no tiles, just
    /// the text. The orchestrator uses it when a session could not start (an
    /// encrypted dump), so the reason is on screen instead of four black tiles.
    explicit Viewer(QVector<Seat> seats, const QString& message = QString(),
                    QWidget* parent = nullptr);

    /// Write the composited window to `path` once the frame counter reaches
    /// `frame`, then quit. QA-only: it is how the composite is checked on a
    /// machine whose compositor is not delivering frames.
    void set_dump(const QString& path, quint64 frame) {
        dump_path_ = path;
        dump_frame_ = frame;
    }
    ~Viewer() override;

    /// Which seat owns the keyboard and mouse.
    int selected() const {
        return selected_;
    }

protected:
    void paintEvent(QPaintEvent* event) override;

    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void open_ring(Seat& seat);
    /// Drop the mapping (a restarted seat replaces the object under it).
    void close_ring(Seat& seat);
    /// True when `/dev/shm/<ring>` is no longer the object this window mapped.
    bool ring_replaced(const Seat& seat) const;
    /// True when the producer resized the frame under us. A seat starts in the
    /// 3DS's single-screen 400x240 mode and switches to 400x480 once the bottom
    /// screen appears, so a seat opened during boot must be re-opened.
    bool ring_resized(const Seat& seat) const;
    bool grab_frame(Seat& seat);
    /// Tile rectangle (in this widget's pixels) each seat is drawn into.
    QRect tile_rect(int index) const;
    /// The console frame inside that tile, keeping the source aspect ratio.
    QRect image_rect(int index) const;
    /// "P1".."P4" in a small translucent tag, so a couch can tell the seats apart.
    void draw_badge(QPainter& painter, int index) const;
    /// Re-read the seat's restart marker; true while the orchestrator restarts it.
    void refresh_restart(int index);
    /// "P2 restarting…" across a tile that is about to go blank, so the couch
    /// knows it is a restart and not a crash.
    void draw_restart_chip(QPainter& painter, int index) const;
    /// Window position -> frame coordinates (the space the seat's touch expects).
    QPoint to_frame(const Seat& seat, const QRect& image, const QPoint& pos) const;
    void send_input(int seat, quint8 type, quint32 key, quint16 x, quint16 y);
    void log_stats();
    void dump_frame();

    QVector<Seat> seats_;
    int selected_ = 0;
    bool dragging_ = false;

    QTimer frame_timer_;
    QElapsedTimer clock_;

    quint64 presented_ = 0;
    quint64 paint_ns_ = 0;
    quint64 ticks_ = 0;
    QString message_;
    QString dump_path_;
    quint64 dump_frame_ = 0;
    qint64 last_stats_ms_ = 0;
};
