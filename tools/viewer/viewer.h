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
#include <QEvent>
#include <QImage>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <SDL.h>

class QPainter;

#include <vector>

/// What each tile shows. `Both` is the stacked pair (top over touch screen);
/// `Top` gives the game's action screen the whole tile, and a player's held peek
/// key shows their touch screen instead, the way the sister project's flip
/// layout works.
enum class View { Both, Top };

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

    /// True while this seat's peek button is held (its F5-F8 key, or its pad's
    /// shoulder). Input truth, independent of layout: the tile only shows the
    /// touch screen when the layout is `Top`.
    bool peek = false;

    /// Where the right stick's touch pointer sits, in this seat's own frame
    /// coordinates (always inside the bottom screen once it exists). Per seat,
    /// so moving focus to another tile does not teleport a cursor, and -1 until
    /// the player first touches the stick.
    float pointer_x = -1;
    float pointer_y = -1;
    /// The last position sent as a touch move, so a stick held against the edge
    /// of the screen does not repeat the same datagram sixty times a second.
    int sent_x = -1;
    int sent_y = -1;
};

/// One gamepad, bound to the seat its player sits at. The seat is fixed for as
/// long as the pad stays plugged in, so a peek always lands on its own tile.
struct Pad {
    SDL_GameController* controller = nullptr; // mapped pad (SDL knows its buttons)
    SDL_Joystick* joystick = nullptr;         // fallback for pads SDL cannot map
    SDL_JoystickID instance = -1;
    int seat = -1;
    QString name;
    /// Bit i is set while kPadKeys[i] is held, so a stick (which reports every
    /// frame) only sends the key changes; the same mask lets an unplugged pad
    /// release everything it was holding instead of leaving a button stuck.
    quint32 buttons = 0;
    bool triggers[2] = {false, false};
    /// Raw left-stick position, as SDL reports it (y down). The seat gets the
    /// circle pad instead: these two are the last values sent, so a resting stick
    /// is quiet and an unplug knows whether to send a neutral position.
    qint16 left_x = 0;
    qint16 left_y = 0;
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

    /// Initial layout; Tab switches it while the session runs. `persist` is
    /// false for a one-run override (`--view`), which must not become the next
    /// session's layout.
    void set_view(View view, bool persist = true);

    /// Where the last used layout is remembered. The default is relative to the
    /// working directory, which the orchestrator sets to the repo root.
    void set_view_file(const QString& path);

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
    /// The part of the seat's 400x480 frame this tile shows right now: the whole
    /// frame in `Both`, the top or (while peeking) the bottom half in `Top`.
    QRect source_rect(int index) const;
    /// The half of the frame the touch screen occupies.
    QRect touch_rect(const Seat& seat) const;
    /// The console frame inside that tile, keeping the source aspect ratio.
    QRect image_rect(int index) const;
    /// Swallow a peek key for the seat that owns it; true when it was one.
    bool handle_peek(int key, bool press);
    /// The part of the frame input maps through: the whole frame while both
    /// screens are shown, the bottom screen when only the top is drawn.
    QRect input_source(int index) const;
    /// Frame position -> window position, the inverse of `to_frame`, so the
    /// pointer is drawn exactly where a click would land.
    QPoint from_frame(int index, const QRect& image, QPointF frame) const;
    /// Put a seat's pointer in the middle of its touch screen the first time it
    /// is needed; it then stays where the player left it.
    void ensure_pointer(int seat);
    void move_pointer(int seat, float dx, float dy);
    void tap_pointer(int seat, bool down);
    /// One stick step for the focused seat at `dt` seconds.
    void apply_stick(float dt);
    /// The pad bound to this seat, or -1.
    int pad_seat(SDL_JoystickID instance) const;
    /// The pad's index in `pads_`, or -1.
    int pad_index(SDL_JoystickID instance) const;
    /// Send a 3DS button press/release to the seat the pad belongs to: a pad
    /// plays its own console, whichever tile has the keyboard focus.
    void pad_button(Pad& pad, int sdl_button, bool down);
    /// Left stick -> circle pad, as the arrow keys the seat profile maps onto the
    /// analog. Only the directions that changed are sent.
    void pad_stick(Pad& pad, int x, int y);
    /// Release everything a pad was holding, so nothing stays pressed.
    void release_pad(Pad& pad);
    void draw_pointer(QPainter& painter, int index);
    /// The one place a peek is set, for keys and pads alike: they must behave
    /// identically, and a peek must never be able to land on another seat.
    void set_peek(int seat, bool held);
    void toggle_view();
    void load_view();
    void save_view() const;
    /// Open the pads already attached and start polling for shoulder presses,
    /// including pads plugged in or pulled out while the session runs.
    void init_pads();
    void poll_pads();
    void add_pad(int device_index);
    void remove_pad(SDL_JoystickID instance);
    bool event(QEvent* event) override;
    /// "P1".."P4" in a small translucent tag, so a couch can tell the seats apart.
    void draw_badge(QPainter& painter, int index) const;
    /// Re-read the seat's restart marker; true while the orchestrator restarts it.
    void refresh_restart(int index);
    /// "P2 restarting…" across a tile that is about to go blank, so the couch
    /// knows it is a restart and not a crash.
    void draw_restart_chip(QPainter& painter, int index) const;
    /// Window position -> frame coordinates (the space the seat's touch expects).
    QPoint to_frame(int index, const QRect& image, const QPoint& pos) const;
    void send_input(int seat, quint8 type, quint32 key, quint16 x, quint16 y);
    void log_stats();
    void dump_frame();

    QVector<Seat> seats_;
    std::vector<Pad> pads_;
    int selected_ = 0;
    bool dragging_ = false;
    View view_ = View::Both;
    QString view_file_ = QStringLiteral("slots/view.txt");
    bool pads_up_ = false;

    /// Right stick, -1..1, already filtered to the focused seat's own pad.
    float stick_x_ = 0;
    float stick_y_ = 0;
    bool tapping_ = false;

    QTimer frame_timer_;
    QTimer pad_timer_;
    QElapsedTimer clock_;
    QElapsedTimer pad_clock_;
    qint64 last_pad_ms_ = 0;
    qint64 last_pointer_log_ms_ = 0;

    quint64 presented_ = 0;
    quint64 paint_ns_ = 0;
    quint64 ticks_ = 0;
    QString message_;
    QString dump_path_;
    quint64 dump_frame_ = 0;
    qint64 last_stats_ms_ = 0;
};
