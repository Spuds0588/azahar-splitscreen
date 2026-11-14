#include "viewer.h"

#include <QCursor>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>

#include <algorithm>
#include <cstring>

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
}

Viewer::~Viewer() {
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
    const QByteArray name = seat.ring_name.toUtf8();
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

QRect Viewer::tile_rect(int index) const {
    const int count = static_cast<int>(seats_.size());
    if (count <= 0) {
        return {};
    }
    const int columns = count >= 3 ? 2 : count;
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
        }
        grab_frame(seat);

        const QRect tile = tile_rect(i);
        if (seat.fd < 0) {
            // No frames yet (still booting): a dim tile with a red border.
            painter.fillRect(tile, QColor(26, 13, 13));
            painter.setPen(QColor(180, 40, 40));
            painter.drawRect(tile.adjusted(0, 0, -1, -1));
            continue;
        }

        const QRect image = image_rect(i);
        // Selected seat: a light frame behind the picture.
        if (i == selected_) {
            painter.fillRect(image.adjusted(-3, -3, 3, 3), QColor(242, 242, 242));
        }
        painter.drawImage(image, seat.canvas);
        draw_badge(painter, i);
    }

    ++presented_;
    if (!dump_path_.isEmpty() && presented_ >= dump_frame_) {
        dump_frame();
    }
    paint_ns_ += clock_.nsecsElapsed() - started;
    log_stats();
}

/// A small tag in the tile's corner: the letterbox margin when the frame does not
/// fill the tile (a 400x480 screen), so nothing of the game is covered, and the
/// picture's own corner when it does. Translucent, so even then the game shows
/// through it.
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
    painter.setPen(QPen(QColor(242, 242, 242, 110), 1));
    painter.setBrush(QColor(18, 18, 24, 200));
    painter.drawRoundedRect(box, pad, pad);
    painter.setPen(QColor(242, 242, 242));
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
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return;
    }
    ::sendto(fd, &message, sizeof(message), MSG_DONTWAIT | MSG_NOSIGNAL,
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
    send_input(selected_, 1, static_cast<quint32>(event->key()), 0, 0);
}

void Viewer::keyReleaseEvent(QKeyEvent* event) {
    if (event->isAutoRepeat()) {
        return;
    }
    send_input(selected_, 2, static_cast<quint32>(event->key()), 0, 0);
}

/// Where a seat's frame is drawn inside its tile: the console screen keeps its
/// aspect ratio and is centred.
QRect Viewer::image_rect(int index) const {
    const QRect tile = tile_rect(index);
    const Seat& seat = seats_[index];
    const float source_aspect =
        static_cast<float>(seat.width) / static_cast<float>(std::max(seat.height, 1u));
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
QPoint Viewer::to_frame(const Seat& seat, const QRect& image, const QPoint& pos) const {
    const float w = static_cast<float>(std::max(seat.width, 1u));
    const float h = static_cast<float>(std::max(seat.height, 1u));
    const int x = static_cast<int>(std::clamp(
        (pos.x() - image.x()) * w / std::max(image.width(), 1), 0.0f, w - 1));
    const int y = static_cast<int>(std::clamp(
        (pos.y() - image.y()) * h / std::max(image.height(), 1), 0.0f, h - 1));
    return {x, y};
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
        selected_ = i;
        update();
        const Seat& seat = seats_[i];
        const QRect image = image_rect(i);
        if (seat.fd >= 0 && image.contains(pos)) {
            const QPoint frame = to_frame(seat, image, pos);
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
    const Seat& seat = seats_[selected_];
    const QPoint frame = to_frame(seat, image_rect(selected_), event->pos());
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
