#include "viewer.h"

#include <QCursor>
#include <QFile>
#include <QFontMetrics>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// Mirrors src/citra_qt/shm_frame.h; duplicated on purpose so the viewer never
// links against the emulator.
struct RingHeader {
    quint32 magic;
    quint32 version;
    quint32 width;
    quint32 height;
    quint32 stride;
    quint32 format;
    quint32 slot_count;
    quint32 slot_bytes;
    quint64 seq;
    quint64 request;
    quint64 reserved[2];
};
static_assert(sizeof(RingHeader) == 64, "ring header must stay 64 bytes");

constexpr quint32 kMagic = 0x48535A41; // 'AZSH'
constexpr quint32 kVersion = 1;
constexpr qsizetype kHeaderBytes = 64;

/// Datagram sent to a seat's input socket.
struct InputMessage {
    quint8 type; // 1 down, 2 up, 3 touch down, 4 touch move, 5 touch up
    quint8 pad[3];
    quint32 key; // Qt::Key for 1/2, unused otherwise
    quint16 x;
    quint16 y;
};
static_assert(sizeof(InputMessage) == 12, "input message must stay 12 bytes");

/// Each player's hold-to-peek key, in seat order. Function keys so they sit
/// well outside the 3DS button keymap: a peek never shadows a game binding, and
/// six players can peek without borrowing a letter key from each other.
constexpr int kPeekKeys[] = {Qt::Key_F5, Qt::Key_F6, Qt::Key_F7,
                             Qt::Key_F8, Qt::Key_F9, Qt::Key_F10};

/// A pad button and the Qt key the seat's input profile binds it to. The codes
/// mirror the `[Controls]` defaults Azahar writes for a fresh user dir (A=65,
/// B=83, X=90, Y=88, L=81, R=87, ZL=49, ZR=50, Start=77, Select=78, Home=66,
/// d-pad 84/71/70/72), and the seat presses them through the forwarded-key path
/// the keyboard already uses, so pads need no new protocol or fork patch.
constexpr PadKey kDefaultPadKeys[] = {
    {SDL_CONTROLLER_BUTTON_A, 65, "A"},
    {SDL_CONTROLLER_BUTTON_B, 83, "B"},
    {SDL_CONTROLLER_BUTTON_X, 90, "X"},
    {SDL_CONTROLLER_BUTTON_Y, 88, "Y"},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, 84, "D-pad up"},
    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 71, "D-pad down"},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 70, "D-pad left"},
    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 72, "D-pad right"},
    {SDL_CONTROLLER_BUTTON_START, 77, "Start"},
    {SDL_CONTROLLER_BUTTON_BACK, 78, "Select"},
    {SDL_CONTROLLER_BUTTON_GUIDE, 66, "Home"},
    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 87, "R"},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, 81, "L (stick click)"},
};
/// Triggers are axes, so they are read with a threshold rather than a button.
constexpr quint32 kTriggerKeys[] = {49, 50}; // ZL, ZR
/// Left stick -> circle pad, sent as its real position rather than four arrow
/// keys, so a half-deflected stick is a half-deflected circle pad. A change of
/// less than this (about 1/64 of the travel) is stick noise and is not sent.
constexpr int kStickStep = 512;

/// Right-stick touch pointer. The deadzone is the usual stick slack (about a
/// fifth of travel), and the speed is in console pixels per second: 320 px/s
/// crosses the 400 px touch screen in 1.25 s, fast enough to reach a menu entry
/// and slow enough to land on one.
constexpr float kStickDeadzone = 0.2f;
constexpr float kTouchSpeed = 320.0f;
/// Taps the touch pointer. The right stick click: the right stick already walks
/// the pointer, and every face button now plays its 3DS button, so tapping must
/// not steal one of those. Left shoulder holds a peek; right shoulder is R.
constexpr int kTapButton = SDL_CONTROLLER_BUTTON_RIGHTSTICK;
constexpr int kPeekButton = SDL_CONTROLLER_BUTTON_LEFTSHOULDER;

void log(const QString& line) {
    QByteArray utf8 = line.toUtf8();
    fprintf(stderr, "%s\n", utf8.constData());
    fflush(stderr);
}

} // namespace

Viewer::Viewer(QVector<Seat> seats, const QString& message, QWidget* parent)
    : QWidget(parent), seats_(std::move(seats)), message_(message) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    clock_.start();
    if (!message_.isEmpty()) {
        return; // a notice paints once and waits for Esc; nothing to poll
    }
    // Three or more seats: the tile is the action screen and a peek reveals the
    // touch screen. Two seats have the room to show both at once.
    view_ = seats_.size() >= 3 ? View::Top : View::Both;
    setCursor(Qt::BlankCursor);

    // Paint on our own clock at roughly 60 Hz. The swap interval paces a healthy
    // display; when it does not (a stalled compositor, or a platform that never
    // waits), this is the pace, so it has to be the display's own rate.
    connect(&frame_timer_, &QTimer::timeout, this, [this] {
        ++ticks_;
        update();
    });
    frame_timer_.start(16);
    for (auto& seat : seats_) {
        open_ring(seat);
    }
    const auto marked = std::count_if(seats_.begin(), seats_.end(), [](const Seat& seat) {
        return !seat.restart_path.isEmpty();
    });
    log(QStringLiteral("viewer: %1 seats, %2 with a restart marker")
            .arg(seats_.size())
            .arg(marked));

    load_view();
    reset_pad_actions();
    load_pads();
    init_pads();
}

Viewer::~Viewer() {
    pad_timer_.stop();
    for (auto& pad : pads_) {
        if (pad.controller != nullptr) {
            SDL_GameControllerClose(pad.controller);
        }
        if (pad.joystick != nullptr) {
            SDL_JoystickClose(pad.joystick);
        }
    }
    pads_.clear();
    if (pads_up_) {
        SDL_Quit();
    }
    for (auto& seat : seats_) {
        if (seat.base != nullptr) {
            munmap(seat.base, static_cast<size_t>(seat.size));
        }
        if (seat.fd >= 0) {
            ::close(seat.fd);
        }
    }
}

// ---------------------------------------------------------------------------
// Frame rings
// ---------------------------------------------------------------------------

void Viewer::open_ring(Seat& seat) {
    // POSIX wants the leading slash; the session passes the plain name.
    QByteArray name = seat.ring_name.toUtf8();
    if (!name.startsWith('/')) {
        name.prepend('/');
    }
    const int fd = shm_open(name.constData(), O_RDONLY, 0);
    if (fd < 0) {
        if (!seat.missing_reported) {
            log(QStringLiteral("viewer: waiting for seat ring %1").arg(seat.ring_name));
            seat.missing_reported = true;
        }
        return;
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size < kHeaderBytes) {
        ::close(fd);
        return; // producer has not sized it yet
    }
    auto* base = static_cast<quint8*>(
        mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0));
    if (base == MAP_FAILED) {
        ::close(fd);
        return;
    }
    auto* header = reinterpret_cast<RingHeader*>(base);
    const qsizetype needed =
        kHeaderBytes + static_cast<qsizetype>(header->slot_count) * header->slot_bytes;
    if (header->magic != kMagic || header->version != kVersion || header->slot_count == 0 ||
        header->slot_bytes == 0 || header->width == 0 || header->height == 0 ||
        info.st_size < needed) {
        // A stale object from a crashed session, or one still being sized: wait
        // for a valid ring rather than adopting whatever is there.
        munmap(base, static_cast<size_t>(info.st_size));
        ::close(fd);
        return;
    }

    seat.fd = fd;
    seat.base = base;
    seat.size = static_cast<qsizetype>(info.st_size);
    seat.width = header->width;
    seat.height = header->height;
    seat.stride = header->stride;
    seat.slot_count = header->slot_count;
    seat.slot_bytes = header->slot_bytes;
    seat.canvas = QImage(static_cast<int>(seat.width), static_cast<int>(seat.height),
                         QImage::Format_ARGB32);
    log(QStringLiteral("viewer: ring %1 is %2x%3, %4 slots")
            .arg(seat.ring_name)
            .arg(seat.width)
            .arg(seat.height)
            .arg(seat.slot_count));
}

void Viewer::close_ring(Seat& seat) {
    if (seat.base != nullptr) {
        munmap(seat.base, static_cast<size_t>(seat.size));
    }
    if (seat.fd >= 0) {
        ::close(seat.fd);
    }
    seat.fd = -1;
    seat.base = nullptr;
    seat.size = 0;
    seat.last_seq = 0;
    seat.missing_reported = false;
    seat.canvas = QImage();
}

/// A restarted seat unlinks its ring and creates a new one under the same name;
/// the mapping this window still holds belongs to the old object, which nobody
/// writes any more. The inode tells the two apart.
bool Viewer::ring_replaced(const Seat& seat) const {
    if (seat.base == nullptr || seat.fd < 0) {
        return false;
    }
    struct stat now {};
    const QByteArray path = QStringLiteral("/dev/shm/%1").arg(seat.ring_name).toUtf8();
    if (::stat(path.constData(), &now) != 0) {
        return false; // gone between frames: the next paint opens whatever appears
    }
    struct stat mapped {};
    if (::fstat(seat.fd, &mapped) != 0) {
        return false;
    }
    return now.st_dev != mapped.st_dev || now.st_ino != mapped.st_ino;
}

bool Viewer::ring_resized(const Seat& seat) const {
    if (seat.base == nullptr) {
        return false;
    }
    const auto* header = reinterpret_cast<const RingHeader*>(seat.base);
    return header->width != seat.width || header->height != seat.height ||
           header->stride != seat.stride || header->slot_count != seat.slot_count;
}

/// Copy the newest complete slot into `canvas`. The producer bumps `seq` with a
/// release store only after the slot is fully written, so a torn frame is
/// detected by re-reading the sequence after the copy.
bool Viewer::grab_frame(Seat& seat) {
    if (seat.base == nullptr) {
        return false;
    }
    auto* header = reinterpret_cast<RingHeader*>(seat.base);
    const quint64 seq = __atomic_load_n(&header->seq, __ATOMIC_ACQUIRE);
    if (seq == 0 || seq == seat.last_seq) {
        return false;
    }
    const quint32 slot = static_cast<quint32>((seq - 1) % seat.slot_count);
    const auto* source = seat.base + kHeaderBytes + static_cast<qsizetype>(slot) * seat.slot_bytes;
    // BGRA, bottom-up: copy rows in reverse so the canvas is top-down.
    for (quint32 row = 0; row < seat.height; ++row) {
        std::memcpy(seat.canvas.scanLine(static_cast<int>(row)),
                    source + static_cast<qsizetype>(seat.height - 1 - row) * seat.stride,
                    seat.width * 4);
    }
    const quint64 after = __atomic_load_n(&header->seq, __ATOMIC_ACQUIRE);
    if (after != seq) {
        return false; // writer moved on mid-copy: drop it rather than show tearing
    }
    seat.last_seq = seq;
    ++seat.frames;
    return true;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

/// The half of the 400x480 frame the touch screen occupies.
QRect Viewer::touch_rect(const Seat& seat) const {
    const int half = static_cast<int>(std::max(seat.height / 2, 1u));
    return {0, static_cast<int>(seat.height) - half, static_cast<int>(seat.width), half};
}

/// What this tile draws right now. In `Both` the tile is the stacked pair, so a
/// peek has nothing to reveal; in `Top` the peek key swaps the halves.
QRect Viewer::source_rect(int index) const {
    const Seat& seat = seats_[index];
    const int width = static_cast<int>(seat.width);
    const int height = static_cast<int>(std::max(seat.height, 1u));
    if (view_ == View::Both) {
        return {0, 0, width, height};
    }
    const QRect bottom = touch_rect(seat);
    return seat.peek ? bottom : QRect(0, 0, width, height - bottom.height());
}

QRect Viewer::tile_rect(int index) const {
    const int count = static_cast<int>(seats_.size());
    if (count <= 0) {
        return {};
    }
    // 2 share a row, 3-4 a 2x2 grid, 5-6 three columns wide.
    const int columns = count >= 5 ? 3 : (count >= 3 ? 2 : count);
    const int rows = (count + columns - 1) / columns;
    const int cell_width = width() / columns;
    const int cell_height = height() / rows;
    const int column = index % columns;
    const int row = index / columns;
    return {column * cell_width, row * cell_height, cell_width, cell_height};
}

void Viewer::paintEvent(QPaintEvent*) {
    const qint64 started = clock_.nsecsElapsed();
    QPainter painter(this);
    painter.fillRect(rect(), QColor(15, 15, 18));
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (!message_.isEmpty()) {
        QFont font = painter.font();
        font.setPixelSize(std::max(16, height() / 34));
        painter.setFont(font);
        painter.setPen(QColor(230, 232, 238));
        painter.drawText(rect().adjusted(width() / 8, height() / 3, -width() / 8, -height() / 3),
                         Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, message_);
        font.setPixelSize(std::max(12, height() / 64));
        painter.setFont(font);
        painter.setPen(QColor(139, 147, 167));
        painter.drawText(rect().adjusted(0, height() * 7 / 10, 0, 0),
                         Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("Press Esc to close."));
        ++presented_;
        paint_ns_ += clock_.nsecsElapsed() - started;
        return;
    }

    for (int i = 0; i < seats_.size(); ++i) {
        Seat& seat = seats_[i];
        if (seat.base == nullptr) {
            open_ring(seat);
        } else if (ring_replaced(seat)) {
            log(QStringLiteral("viewer: ring %1 was replaced, re-attaching").arg(seat.ring_name));
            close_ring(seat);
            open_ring(seat);
        } else if (ring_resized(seat)) {
            // The seat switched from the boot screen to the real one: the header
            // is the new size and the canvas has to follow it.
            close_ring(seat);
            open_ring(seat);
        }
        refresh_restart(i);
        grab_frame(seat);

        const QRect tile = tile_rect(i);
        if (seat.fd < 0) {
            // No frames yet (still booting): a dim tile in the player's color.
            painter.fillRect(tile, QColor(26, 13, 13));
            draw_outline(painter, i);
            draw_restart_chip(painter, i);
            continue;
        }

        const QRect image = image_rect(i);
        // Selected seat: a light frame behind the picture.
        if (i == selected_) {
            painter.fillRect(image.adjusted(-3, -3, 3, 3), QColor(242, 242, 242));
        }
        painter.drawImage(image, seat.canvas, source_rect(i));
        if (i == selected_) {
            draw_pointer(painter, i);
        }
        draw_outline(painter, i);
        draw_badge(painter, i);
        draw_restart_chip(painter, i);
    }

    ++presented_;
    if (!dump_path_.isEmpty() && presented_ >= dump_frame_) {
        dump_frame();
    }
    paint_ns_ += clock_.nsecsElapsed() - started;
    log_stats();
}

/// The player's identity color, from mgba-splitscreen's palette (`styles.css`):
/// P1 red, P2 blue, P3 green, P4 orange. Purple and cyan extend it for the
/// fifth and sixth seat.
static QColor player_color(int index) {
    static const QColor palette[] = {
        {0xff, 0x44, 0x44}, {0x44, 0x88, 0xff}, {0x2e, 0xaa, 0x44},
        {0xe0, 0x8a, 0x1e}, {0x9b, 0x5c, 0xff}, {0x24, 0xc8, 0xdb},
    };
    return palette[index % 6];
}

/// mgba-splitscreen's outline rule: the player's color drawn three pixels inset
/// from the cell's own edge, after the picture, so it survives any layout at any
/// scale - a letterboxed portrait screen gets its whole cell outlined, and nothing
/// depends on the picture's size.
void Viewer::draw_outline(QPainter& painter, int index) const {
    const QRect cell = tile_rect(index);
    if (cell.isEmpty()) {
        return;
    }
    const int edge = 3;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setBrush(player_color(index));
    painter.drawRect(QRect(cell.left(), cell.top(), cell.width(), edge));
    painter.drawRect(QRect(cell.left(), cell.bottom() - edge + 1, cell.width(), edge));
    painter.drawRect(QRect(cell.left(), cell.top(), edge, cell.height()));
    painter.drawRect(QRect(cell.right() - edge + 1, cell.top(), edge, cell.height()));
    painter.restore();
}

/// A small tag in the tile's corner: the letterbox margin when the frame does not
/// fill the tile (a 400x480 screen), so nothing of the game is covered, and the
/// picture's own corner when it does. Filled in the player's color, like
/// mgba-splitscreen's player tag, so the tag and the outline name the same seat.
void Viewer::draw_badge(QPainter& painter, int index) const {
    const QRect tile = tile_rect(index);
    const QRect image = image_rect(index);
    const QString label = QStringLiteral("P%1").arg(index + 1);

    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(std::max(14, tile.height() / 26));
    const QFontMetrics metrics(font);
    const int pad = std::max(5, font.pixelSize() / 3);
    const int box_width = metrics.horizontalAdvance(label) + pad * 2;
    const int box_height = metrics.height() + pad;
    // Sit right beside the picture, in the letterbox margin a portrait screen
    // leaves, so the tag never covers the game; with no margin, go into the
    // picture's own top-left corner.
    int x = image.left() - box_width - pad;
    if (x < tile.left() + pad) {
        x = image.left() + pad;
    }
    const QRect box(x, image.top() + pad, box_width, box_height);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font);
    painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
    painter.setBrush(player_color(index));
    painter.drawRoundedRect(box, pad, pad);
    painter.setPen(QColor(255, 255, 255));
    painter.drawText(box, Qt::AlignCenter, label);
    painter.restore();
}

void Viewer::refresh_restart(int index) {
    Seat& seat = seats_[index];
    if (seat.restart_path.isEmpty()) {
        return;
    }
    const QByteArray path = seat.restart_path.toUtf8();
    // The orchestrator removes the marker when the seat publishes again, so the
    // file's presence is the whole signal; no timer to guess at here.
    const bool restarting = ::access(path.constData(), F_OK) == 0;
    if (restarting != seat.restarting) {
        const QRect tile = tile_rect(index);
        log(QStringLiteral("viewer: %1 %2 (tile %3x%4)")
                .arg(seat.ring_name,
                     restarting ? QStringLiteral("restarting") : QStringLiteral("frames resumed"))
                .arg(tile.width())
                .arg(tile.height()));
    }
    seat.restarting = restarting;
}

/// A centred pill over the tile: the seat is coming back, not broken. It is the
/// one thing drawn on top of the picture, and only while the tile is out.
void Viewer::draw_restart_chip(QPainter& painter, int index) const {
    if (!seats_[index].restarting) {
        return;
    }
    const QRect tile = tile_rect(index);
    const QString label = QStringLiteral("P%1 restarting…").arg(index + 1);

    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(std::max(14, tile.height() / 22));
    const QFontMetrics metrics(font);
    const int pad = std::max(8, font.pixelSize() / 2);
    const QRect box(tile.x() + (tile.width() - (metrics.horizontalAdvance(label) + pad * 3)) / 2,
                    tile.y() + (tile.height() - (metrics.height() + pad)) / 2,
                    metrics.horizontalAdvance(label) + pad * 3, metrics.height() + pad);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setFont(font);
    painter.setPen(QPen(QColor(230, 180, 90, 150), 1));
    painter.setBrush(QColor(20, 16, 10, 225));
    painter.drawRoundedRect(box, pad, pad);
    painter.setPen(QColor(245, 225, 190));
    painter.drawText(box, Qt::AlignCenter, label);
    painter.restore();
}

void Viewer::dump_frame() {
    const QString path = dump_path_;
    dump_path_.clear(); // grab() repaints; do not recurse into this dump
    const QImage image = grab().toImage();
    if (!image.save(path)) {
        log(QStringLiteral("viewer: could not write %1").arg(path));
    } else {
        log(QStringLiteral("viewer: wrote %1 (%2x%3)").arg(path).arg(image.width()).arg(image.height()));
    }
    QTimer::singleShot(0, this, [this] { close(); });
}

void Viewer::log_stats() {
    const qint64 elapsed = clock_.elapsed();
    if (elapsed - last_stats_ms_ < 1000) {
        return;
    }
    const double seconds = (elapsed - last_stats_ms_) / 1000.0;
    last_stats_ms_ = elapsed;
    QString line = QStringLiteral("viewer: ticks=%1 presented_fps=%2 paint_ms=%3")
                       .arg(ticks_ / seconds, 0, 'f', 0)
                       .arg(presented_ / seconds, 0, 'f', 1)
                       .arg(paint_ns_ / 1e6 / std::max<quint64>(presented_, 1), 0, 'f', 2);
    ticks_ = 0;
    QString unused = QStringLiteral("viewer: presented_fps=%1 paint_ms=%2")
                       .arg(presented_ / seconds, 0, 'f', 1)
                       .arg(paint_ns_ / 1e6 / std::max<quint64>(presented_, 1), 0, 'f', 2);
    paint_ns_ = 0;
    for (int i = 0; i < seats_.size(); ++i) {
        line += QStringLiteral(" seat%1_seq=%2").arg(i).arg(seats_[i].last_seq);
    }
    log(line);
    presented_ = 0;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void Viewer::send_input(int seat, quint8 type, quint32 key, quint16 x, quint16 y) {
    if (seat < 0 || seat >= seats_.size() || seats_[seat].socket_path.isEmpty()) {
        return;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const QByteArray path = seats_[seat].socket_path.toUtf8();
    if (path.size() >= static_cast<int>(sizeof(address.sun_path))) {
        return;
    }
    std::memcpy(address.sun_path, path.constData(), static_cast<size_t>(path.size()));

    InputMessage message {};
    message.type = type;
    message.key = key;
    message.x = x;
    message.y = y;

    // The instance owns the socket; if it is not up yet (still booting) the
    // datagram is simply dropped.
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    // SOCK_CLOEXEC is a Linux-only socket() flag and MSG_NOSIGNAL has no macOS
    // equivalent: fcntl plus main's ignored SIGPIPE cover both platforms.
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = MSG_DONTWAIT;
#endif
    ::sendto(fd, &message, sizeof(message), kSendFlags,
             reinterpret_cast<sockaddr*>(&address), sizeof(address));
    ::close(fd);
}

void Viewer::keyPressEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    if (handle_peek(event->key(), true)) {
        return;
    }
    send_input(selected_, 1, static_cast<quint32>(event->key()), 0, 0);
}

void Viewer::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        return;
    }
    if (handle_peek(event->key(), false)) {
        return;
    }
    send_input(selected_, 2, static_cast<quint32>(event->key()), 0, 0);
}

/// Where a seat's frame is drawn inside its tile: the console screen keeps its
/// aspect ratio and is centred.
QRect Viewer::image_rect(int index) const {
    const QRect tile = tile_rect(index);
    const QRect source = source_rect(index);
    const float source_aspect =
        static_cast<float>(source.width()) / static_cast<float>(std::max(source.height(), 1));
    QRect image = tile;
    if (tile.width() * 1.0 / tile.height() > source_aspect) {
        image.setWidth(static_cast<int>(tile.height() * source_aspect));
        image.moveLeft(tile.x() + (tile.width() - image.width()) / 2);
    } else {
        image.setHeight(static_cast<int>(tile.width() / source_aspect));
        image.moveTop(tile.y() + (tile.height() - image.height()) / 2);
    }
    return image;
}

/// Window position -> the seat's own frame coordinates, which is the space its
/// touch handling expects (the seat maps those through its screen layout).
/// When only the top screen is shown, the picture is the pen surface for the
/// touch screen: touch must not depend on what is visible, so a click maps onto
/// the bottom half at the same relative spot.
QRect Viewer::input_source(int index) const {
    // While both screens are drawn the whole frame is the surface; in top-only
    // the picture stands in for the touch screen, so a relative position on it
    // is the same relative position on the bottom screen.
    return view_ == View::Top ? touch_rect(seats_[index]) : source_rect(index);
}

QPoint Viewer::from_frame(int index, const QRect& image, QPointF frame) const {
    const QRect source = input_source(index);
    const float rx = static_cast<float>(frame.x() - source.x()) / std::max(source.width(), 1);
    const float ry = static_cast<float>(frame.y() - source.y()) / std::max(source.height(), 1);
    return {image.x() + static_cast<int>(rx * image.width()),
            image.y() + static_cast<int>(ry * image.height())};
}

QPoint Viewer::to_frame(int index, const QRect& image, const QPoint& pos) const {
    const QRect source = input_source(index);
    const float w = static_cast<float>(std::max(source.width(), 1));
    const float h = static_cast<float>(std::max(source.height(), 1));
    const int x = static_cast<int>(std::clamp(
        source.x() + (pos.x() - image.x()) * w / std::max(image.width(), 1), 0.0f,
        static_cast<float>(source.right())));
    const int y = static_cast<int>(std::clamp(
        source.y() + (pos.y() - image.y()) * h / std::max(image.height(), 1), 0.0f,
        static_cast<float>(source.bottom())));
    return {x, y};
}

// ---------------------------------------------------------------------------
// Right-stick touch pointer
// ---------------------------------------------------------------------------

void Viewer::ensure_pointer(int seat) {
    if (seat < 0 || seat >= seats_.size() || seats_[seat].pointer_x >= 0) {
        return;
    }
    const QRect bottom = touch_rect(seats_[seat]);
    seats_[seat].pointer_x = bottom.x() + bottom.width() / 2.0f;
    seats_[seat].pointer_y = bottom.y() + bottom.height() / 2.0f;
}

/// One stick step. The pointer is clamped to the touch screen and only this
/// seat's socket is told about it, so a second seat never sees the motion.
void Viewer::move_pointer(int seat, float dx, float dy) {
    ensure_pointer(seat);
    const QRect bottom = touch_rect(seats_[seat]);
    Seat& target = seats_[seat];
    target.pointer_x = std::clamp(target.pointer_x + dx, static_cast<float>(bottom.left()),
                                  static_cast<float>(bottom.left() + bottom.width() - 1));
    target.pointer_y = std::clamp(target.pointer_y + dy, static_cast<float>(bottom.top()),
                                  static_cast<float>(bottom.top() + bottom.height() - 1));
    const int x = static_cast<int>(target.pointer_x);
    const int y = static_cast<int>(target.pointer_y);
    if (x == target.sent_x && y == target.sent_y) {
        return; // against an edge: the pointer cannot move further
    }
    target.sent_x = x;
    target.sent_y = y;
    send_input(seat, 4, 0, static_cast<quint16>(x), static_cast<quint16>(y));

    const qint64 now = clock_.elapsed();
    if (now - last_pointer_log_ms_ > 250) {
        last_pointer_log_ms_ = now;
        log(QStringLiteral("viewer: P%1 touch pointer (%2,%3)").arg(seat + 1).arg(x).arg(y));
    }
    update();
}

void Viewer::tap_pointer(int seat, bool down) {
    if (seat < 0 || seat >= seats_.size()) {
        return;
    }
    ensure_pointer(seat);
    const int x = static_cast<int>(seats_[seat].pointer_x);
    const int y = static_cast<int>(seats_[seat].pointer_y);
    send_input(seat, down ? 3 : 5, 0, down ? static_cast<quint16>(x) : 0,
               down ? static_cast<quint16>(y) : 0);
    log(QStringLiteral("viewer: P%1 tap %2 (%3,%4)")
            .arg(seat + 1)
            .arg(down ? QStringLiteral("down") : QStringLiteral("up"))
            .arg(x)
            .arg(y));
}

/// Quadratic-feeling response: tiny deflections barely move, full deflection
/// crosses the screen in about a second. Below the deadzone nothing happens, so
/// a centred stick never drifts.
void Viewer::apply_stick(float dt) {
    const float magnitude = std::hypot(stick_x_, stick_y_);
    if (magnitude <= kStickDeadzone || selected_ < 0 || selected_ >= seats_.size()) {
        return;
    }
    const float speed = kTouchSpeed * (magnitude - kStickDeadzone) / (1.0f - kStickDeadzone);
    move_pointer(selected_, stick_x_ / magnitude * speed * dt, stick_y_ / magnitude * speed * dt);
}

int Viewer::pad_seat(SDL_JoystickID instance) const {
    for (const auto& pad : pads_) {
        if (pad.instance == instance) {
            return pad.seat;
        }
    }
    return -1;
}

int Viewer::pad_index(SDL_JoystickID instance) const {
    for (int i = 0; i < static_cast<int>(pads_.size()); ++i) {
        if (pads_[i].instance == instance) {
            return i;
        }
    }
    return -1;
}

/// Buttons go to the seat that owns the pad, not to the focused tile: a player
/// pressing A is pressing their own 3DS's A. The key travels as the same
/// forwarded key the keyboard sends, so the seat's own profile decides what it
/// means.
void Viewer::pad_button(Pad& pad, int sdl_button, bool down) {
    for (int i = 0; i < pad_keys_.size(); ++i) {
        if (pad_keys_[i].button != sdl_button) {
            continue;
        }
        const quint32 bit = 1u << i;
        if (((pad.buttons & bit) != 0) == down) {
            return; // already in that state
        }
        pad.buttons = down ? (pad.buttons | bit) : (pad.buttons & ~bit);
        send_input(pad.seat, down ? 1 : 2, pad_keys_[i].key, 0, 0);
        return;
    }
}

/// The left stick is the circle pad, with the deflection it was given. SDL hands
/// over -32768..32767 with y pointing down; the seat takes the input layer's
/// analog convention instead (x right, y up), so y is flipped here and the seat
/// scales both to the console's range.
void Viewer::pad_stick(Pad& pad, int x, int y) {
    const int up = -std::clamp(y, -32767, 32767);
    if (std::abs(x - pad.left_x) < kStickStep && std::abs(up - pad.left_y) < kStickStep) {
        return;
    }
    pad.left_x = static_cast<qint16>(x);
    pad.left_y = static_cast<qint16>(up);
    send_input(pad.seat, 6, 0, static_cast<quint16>(pad.left_x),
               static_cast<quint16>(pad.left_y));
}

/// An unplugged pad must not leave a button held in the console, and neither may
/// a viewer that is closing.
void Viewer::release_pad(Pad& pad) {
    for (int i = 0; i < pad_keys_.size(); ++i) {
        if (pad.buttons & (1u << i)) {
            send_input(pad.seat, 2, pad_keys_[i].key, 0, 0);
        }
    }
    if (pad.left_x != 0 || pad.left_y != 0) {
        send_input(pad.seat, 6, 0, 0, 0); // centred: the pad left the circle pad
    }
    for (int i = 0; i < 2; ++i) {
        if (pad.triggers[i]) {
            send_input(pad.seat, 2, kTriggerKeys[i], 0, 0);
        }
    }
    pad.buttons = 0;
    pad.left_x = 0;
    pad.left_y = 0;
    pad.triggers[0] = false;
    pad.triggers[1] = false;
}

/// Drawn through the same mapping a click uses, so what is aimed at is what the
/// tap hits. Only the selected tile shows one.
void Viewer::draw_pointer(QPainter& painter, int index) {
    const Seat& seat = seats_[index];
    if (seat.pointer_x < 0) {
        return;
    }
    const QPoint at = from_frame(index, image_rect(index), {seat.pointer_x, seat.pointer_y});
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(0, 0, 0, 170), 3));
    painter.drawEllipse(at, 8, 8);
    painter.setPen(QPen(QColor(255, 255, 255, 230), 2));
    painter.drawEllipse(at, 7, 7);
    painter.drawLine(at.x() - 3, at.y(), at.x() + 3, at.y());
    painter.drawLine(at.x(), at.y() - 3, at.x(), at.y() + 3);
    painter.restore();
}

void Viewer::set_view(View view, bool persist) {
    view_ = view;
    log(QStringLiteral("viewer: view %1")
            .arg(view_ == View::Top ? QStringLiteral("top screen only")
                                    : QStringLiteral("both screens")));
    if (persist) {
        save_view();
    }
    update();
}

void Viewer::set_view_file(const QString& path) {
    view_file_ = path;
    load_view();
    update();
}

/// The layout the last session ended in, so the couch never has to ask for it.
/// A missing or unreadable file simply means "start stacked", as before.
void Viewer::load_view() {
    if (view_file_.isEmpty() || seats_.isEmpty()) {
        return;
    }
    QFile file(view_file_);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    // Three or more seats are top-screen-only by design; a saved layout from a
    // two-seat session must not open them stacked.
    if (seats_.size() >= 3) {
        return;
    }
    const QString saved = QString::fromUtf8(file.readLine(32)).trimmed();
    if (saved == QLatin1String("top")) {
        view_ = View::Top;
    } else if (saved == QLatin1String("both")) {
        view_ = View::Both;
    } else {
        return;
    }
    log(QStringLiteral("viewer: resuming %1 from %2")
            .arg(view_ == View::Top ? QStringLiteral("the top screen")
                                    : QStringLiteral("both screens"))
            .arg(view_file_));
}

void Viewer::save_view() const {
    if (view_file_.isEmpty() || seats_.isEmpty()) {
        return; // a notice window has no session layout to remember
    }
    QFile file(view_file_);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write(view_ == View::Top ? "top\n" : "both\n");
    }
}

// ---------------------------------------------------------------------------
// Pad mapping
// ---------------------------------------------------------------------------

QVector<PadAction> Viewer::pad_actions() const {
    QVector<PadAction> actions;
    actions.reserve(pad_keys_.size());
    for (const auto& key : pad_keys_) {
        actions.push_back({QString::fromLatin1(key.name), key.button});
    }
    return actions;
}

/// One button can drive one action: assigning a button another action already
/// holds unbinds that action instead of leaving an unreachable duplicate.
void Viewer::set_pad_action(int index, int button) {
    if (index < 0 || index >= pad_keys_.size()) {
        return;
    }
    for (int i = 0; i < pad_keys_.size(); ++i) {
        if (i != index && pad_keys_[i].button == button) {
            pad_keys_[i].button = -1;
        }
    }
    pad_keys_[index].button = button;
    save_pads();
    log(QStringLiteral("viewer: %1 -> %2")
            .arg(QString::fromLatin1(pad_keys_[index].name))
            .arg(QString::number(button)));
}

void Viewer::reset_pad_actions() {
    pad_keys_.clear();
    for (const auto& key : kDefaultPadKeys) {
        pad_keys_.push_back(key);
    }
}

void Viewer::listen_for_pad_button(std::function<void(int)> sink) {
    pad_listen_ = std::move(sink);
}

void Viewer::cancel_pad_listen() {
    pad_listen_ = nullptr;
}

/// `buttons` is indexed like the built-in table, so a saved mapping survives a
/// build that adds or reorders nothing; a size mismatch means the file predates
/// this table and the defaults stay.
void Viewer::load_pads() {
    QFile file(pad_file_);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }
    const QJsonArray buttons = QJsonDocument::fromJson(file.readAll()).object().value("buttons").toArray();
    if (buttons.size() != pad_keys_.size()) {
        return;
    }
    for (int i = 0; i < pad_keys_.size(); ++i) {
        pad_keys_[i].button = buttons[i].toInt(pad_keys_[i].button);
    }
    log(QStringLiteral("viewer: pad mapping loaded from %1").arg(pad_file_));
}

void Viewer::save_pads() const {
    if (seats_.isEmpty()) {
        return;
    }
    QJsonArray buttons;
    for (const auto& key : pad_keys_) {
        buttons.append(key.button);
    }
    QJsonObject root;
    root.insert("buttons", buttons);
    QFile file(pad_file_);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    }
}

/// A held peek key belongs to its own seat, whichever seat has the keyboard.
bool Viewer::handle_peek(int key, bool press) {
    const int seats = static_cast<int>(seats_.size());
    for (int seat = 0; seat < seats && seat < static_cast<int>(std::size(kPeekKeys)); ++seat) {
        if (key != kPeekKeys[seat]) {
            continue;
        }
        set_peek(seat, press);
        return true;
    }
    return false;
}

void Viewer::set_peek(int seat, bool held) {
    if (seat < 0 || seat >= seats_.size()) {
        return;
    }
    if (seats_[seat].peek == held) {
        return;
    }
    seats_[seat].peek = held;
    log(QStringLiteral("viewer: P%1 %2")
            .arg(seat + 1)
            .arg(held ? "peeking at the touch screen" : "back to the top screen"));
    update();
}

void Viewer::toggle_view() {
    set_view(view_ == View::Both ? View::Top : View::Both);
}

// ---------------------------------------------------------------------------
// Pads
// ---------------------------------------------------------------------------

/// Each pad owns the seat it was open for, so four people on four pads never
/// need to touch the keyboard. A shoulder held on one pad is that seat's peek:
/// the same `set_peek` the F5-F8 keys use, so both paths behave identically in
/// either layout.
void Viewer::init_pads() {
    if (seats_.isEmpty()) {
        return;
    }
    pad_clock_.start();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        log(QStringLiteral("viewer: no pad input (%1)").arg(SDL_GetError()));
        return;
    }
    pads_up_ = true;
    const int attached = SDL_NumJoysticks();
    for (int i = 0; i < attached; ++i) {
        add_pad(i);
    }
    if (pads_.empty()) {
        log(QStringLiteral("viewer: no pads attached, F5-F%1 peek for seats 1-%2")
                .arg(4 + static_cast<int>(seats_.size()))
                .arg(seats_.size()));
    }
    // The frame timer is the pace of this window, so polling pads twice per frame
    // buys nothing a player can feel; half that rate keeps a held shoulder
    // responsive while leaving the paint loop alone.
    connect(&pad_timer_, &QTimer::timeout, this, [this] { poll_pads(); });
    pad_timer_.start(16);
}

/// The lowest seat without a pad. Pads are numbered by the order they appear,
/// which is how players find them: the first pad is P1, the second P2.
static int free_seat(const std::vector<Pad>& pads, int seats) {
    for (int seat = 0; seat < seats; ++seat) {
        const bool taken = std::any_of(pads.begin(), pads.end(),
                                       [seat](const Pad& pad) { return pad.seat == seat; });
        if (!taken) {
            return seat;
        }
    }
    return -1;
}

void Viewer::add_pad(int device_index) {
    // SDL queues an add event for every device already attached, so a pad opened
    // at startup arrives here again. One handle per pad: otherwise the same pad
    // would own a second seat and its shoulder would peek two tiles at once.
    const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(device_index);
    if (instance >= 0 &&
        std::any_of(pads_.begin(), pads_.end(),
                    [instance](const Pad& pad) { return pad.instance == instance; })) {
        return;
    }
    const int seat = free_seat(pads_, static_cast<int>(seats_.size()));
    if (seat < 0) {
        return; // every seat already has a pad
    }
    Pad pad;
    pad.seat = seat;
    if (SDL_IsGameController(device_index)) {
        pad.controller = SDL_GameControllerOpen(device_index);
        if (pad.controller != nullptr) {
            pad.joystick = SDL_GameControllerGetJoystick(pad.controller);
        }
    }
    if (pad.joystick == nullptr) {
        // SDL has no mapping for this pad; its shoulders are still buttons 4/5
        // on every pad this project has met, so peek works anyway.
        pad.joystick = SDL_JoystickOpen(device_index);
    }
    if (pad.joystick == nullptr) {
        log(QStringLiteral("viewer: could not open pad %1 (%2)")
                .arg(device_index)
                .arg(SDL_GetError()));
        return;
    }
    pad.instance = SDL_JoystickInstanceID(pad.joystick);
    pad.name = QString::fromUtf8(SDL_JoystickNameForIndex(device_index));
    log(QStringLiteral("viewer: pad %1 \"%2\" -> P%3")
            .arg(device_index)
            .arg(pad.name)
            .arg(seat + 1));
    pads_.push_back(pad);
}

/// Unplugging a pad mid-peek must not leave its tile on the touch screen, so
/// the seat's peek is released with it.
void Viewer::remove_pad(SDL_JoystickID instance) {
    for (auto it = pads_.begin(); it != pads_.end(); ++it) {
        if (it->instance != instance) {
            continue;
        }
        set_peek(it->seat, false);
        release_pad(*it);
        if (it->seat == selected_) {
            stick_x_ = 0; // a stick that vanished must not keep steering
            stick_y_ = 0;
        }
        log(QStringLiteral("viewer: P%1 pad \"%2\" unplugged").arg(it->seat + 1).arg(it->name));
        if (it->controller != nullptr) {
            SDL_GameControllerClose(it->controller);
        }
        if (it->joystick != nullptr) {
            SDL_JoystickClose(it->joystick);
        }
        pads_.erase(it);
        return;
    }
}

void Viewer::poll_pads() {
    SDL_Event event;
    while (SDL_PollEvent(&event) != 0) {
        switch (event.type) {
        case SDL_CONTROLLERDEVICEADDED:
            add_pad(event.cdevice.which);
            break;
        case SDL_JOYDEVICEADDED:
            if (!SDL_IsGameController(event.jdevice.which)) {
                add_pad(event.jdevice.which);
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
        case SDL_JOYDEVICEREMOVED:
            remove_pad(event.type == SDL_CONTROLLERDEVICEREMOVED ? event.cdevice.which
                                                                 : event.jdevice.which);
            break;
        case SDL_CONTROLLERAXISMOTION: {
            const int index = pad_index(event.caxis.which);
            if (index < 0) {
                break;
            }
            Pad& pad = pads_[index];
            const float value = std::clamp(event.caxis.value / 32767.0f, -1.0f, 1.0f);
            switch (event.caxis.axis) {
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: {
                const int which = event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? 0 : 1;
                const bool down = value > 0.5f;
                if (pad.triggers[which] != down) {
                    pad.triggers[which] = down;
                    send_input(pad.seat, down ? 1 : 2, kTriggerKeys[which], 0, 0);
                }
                break;
            }
            case SDL_CONTROLLER_AXIS_LEFTX:
                pad_stick(pad, event.caxis.value, pad.left_y);
                break;
            case SDL_CONTROLLER_AXIS_LEFTY:
                pad_stick(pad, pad.left_x, event.caxis.value);
                break;
            case SDL_CONTROLLER_AXIS_RIGHTX:
            case SDL_CONTROLLER_AXIS_RIGHTY:
                // Only the pad that owns the focused tile steers that tile's
                // pointer, so two pads fighting over one cursor is impossible.
                if (pad.seat != selected_) {
                    break;
                }
                if (event.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX) {
                    stick_x_ = value;
                } else {
                    stick_y_ = value;
                }
                break;
            default:
                break;
            }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const int index = pad_index(event.cbutton.which);
            if (index < 0) {
                break;
            }
            Pad& pad = pads_[index];
            const int button = event.cbutton.button;
            const bool pressed = event.type == SDL_CONTROLLERBUTTONDOWN;
            if (pressed && pad_listen_) {
                // The mapping dialog asked for the next press; it must not also
                // play the console.
                auto sink = std::move(pad_listen_);
                pad_listen_ = nullptr;
                sink(button);
                break;
            }
            if (button == kPeekButton) {
                set_peek(pad.seat, pressed);
            } else if (button == kTapButton) {
                if (pad.seat == selected_ && pressed != tapping_) {
                    tapping_ = pressed;
                    tap_pointer(pad.seat, pressed);
                }
            } else {
                // Everything else plays that pad's own console: the viewer has no
                // saying in which tile is focused for gameplay input.
                pad_button(pad, button, pressed);
            }
            break;
        }
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
            // Shoulders are buttons 4 and 5 on the pads SDL cannot map.
            if (event.jbutton.button != 4 && event.jbutton.button != 5) {
                break;
            }
            for (const auto& pad : pads_) {
                if (pad.controller == nullptr && pad.joystick != nullptr &&
                    pad.instance == event.jbutton.which) {
                    set_peek(pad.seat, event.type == SDL_JOYBUTTONDOWN);
                    break;
                }
            }
            break;
        default:
            break;
        }
    }

    const qint64 now = pad_clock_.elapsed();
    const float dt = std::clamp((now - last_pad_ms_) / 1000.0f, 0.0f, 0.1f);
    last_pad_ms_ = now;
    apply_stick(dt);
}

/// Tab changes the layout, so it is taken here before Qt can route it as focus
/// navigation; every other key goes on to the selected seat. Losing the window
/// drops every held peek: a key release that happens while another window has
/// the keyboard is never delivered here, and a peek must not stick.
bool Viewer::event(QEvent* event) {
    if (event->type() == QEvent::KeyPress) {
        auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Tab && !key->isAutoRepeat()) {
            toggle_view();
            return true;
        }
    } else if (event->type() == QEvent::WindowDeactivate) {
        for (int seat = 0; seat < seats_.size(); ++seat) {
            set_peek(seat, false);
        }
        // A stick held while another window has focus would otherwise keep
        // walking the pointer, and an un-ended tap would never release.
        stick_x_ = 0;
        stick_y_ = 0;
        if (tapping_) {
            tapping_ = false;
            tap_pointer(selected_, false);
        }
    }
    return QWidget::event(event);
}

void Viewer::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPoint pos = event->pos();
    for (int i = 0; i < seats_.size(); ++i) {
        if (!tile_rect(i).contains(pos)) {
            continue;
        }
        if (selected_ != i) {
            stick_x_ = 0; // the previous seat's pad must not steer this one
            stick_y_ = 0;
        }
        selected_ = i;
        update();
        const Seat& seat = seats_[i];
        const QRect image = image_rect(i);
        if (seat.fd >= 0 && image.contains(pos)) {
            const QPoint frame = to_frame(i, image, pos);
            dragging_ = true;
            send_input(i, 3, 0, static_cast<quint16>(frame.x()),
                       static_cast<quint16>(frame.y()));
        }
        return;
    }
}

void Viewer::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        return;
    }
    const QPoint frame = to_frame(selected_, image_rect(selected_), event->pos());
    send_input(selected_, 4, 0, static_cast<quint16>(frame.x()),
               static_cast<quint16>(frame.y()));
}

void Viewer::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !dragging_) {
        return;
    }
    dragging_ = false;
    send_input(selected_, 5, 0, 0, 0);
}
