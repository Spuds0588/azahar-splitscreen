// azahar-viewer — one window for a split-screen Azahar session.
//
//   azahar-viewer --seat azsh-1:/tmp/p1.sock --seat azsh-2:/tmp/p2.sock …
//   azahar-viewer --windowed --seat azsh-1
//
// Each --seat is `<shm ring name>[:<input socket>]`. The viewer draws one tile
// per ring in a near-square grid and forwards keyboard/touch input to the
// selected seat (click a tile to select it, Escape quits). Fullscreen and
// frameless by default — a couch session should be one window and nothing else.

#include "viewer.h"

#include <QApplication>
#include <QHash>
#include <QScreen>
#include <QSurfaceFormat>

#include <csignal>

int main(int argc, char* argv[]) {
    // A seat that exits while input is still flowing must not take the viewer
    // down with a SIGPIPE; send_input already ignores the failure.
    std::signal(SIGPIPE, SIG_IGN);

    // A compositor that misses a frame callback for longer than Qt's default
    // timeout inexposes the window and Qt stops painting it for good. The viewer
    // paces itself with its own timer, so it must not depend on that handshake;
    // a healthy compositor answers in milliseconds and never notices this.
    if (qEnvironmentVariableIsEmpty("QT_WAYLAND_FRAME_CALLBACK_TIMEOUT")) {
        qputenv("QT_WAYLAND_FRAME_CALLBACK_TIMEOUT", "60000");
    }

    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(0);
    format.setStencilBufferSize(0);
    // Paced by the viewer's own timer, not by the compositor: on this machine a
    // compositor that is not delivering frame callbacks (window off-screen, or
    // simply not composited yet) blocks the swap to ~1 Hz and starves the whole
    // window. The timer is the pacing authority; a late frame is skipped.
    format.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("azahar-viewer"));

    QVector<Seat> seats;
    // `<ring>=<marker file>`, matched to the seats after all arguments are read.
    QHash<QString, QString> restart_paths;
    QString message;
    QString view_mode;
    QString view_file;
    QString title = QStringLiteral("Azahar Splitscreen");
    QString dump_path;
    quint64 dump_frame = 0;
    bool windowed = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if ((arg == QLatin1String("--seat") || arg == QLatin1String("--ring")) && i + 1 < argc) {
            const QString spec = QString::fromLocal8Bit(argv[++i]);
            const int colon = spec.indexOf(QLatin1Char(':'));
            Seat seat;
            seat.ring_name = colon > 0 ? spec.left(colon) : spec;
            seat.socket_path = colon > 0 ? spec.mid(colon + 1) : QString();
            if (!seat.ring_name.isEmpty()) {
                seats.push_back(seat);
            }
        } else if (arg == QLatin1String("--restart") && i + 1 < argc) {
            const QString spec = QString::fromLocal8Bit(argv[++i]);
            const int eq = spec.indexOf(QLatin1Char('='));
            if (eq > 0) {
                restart_paths.insert(spec.left(eq), spec.mid(eq + 1));
            }
        } else if (arg == QLatin1String("--dump") && i + 2 < argc) {
            dump_path = QString::fromLocal8Bit(argv[++i]);
            dump_frame = QString::fromLocal8Bit(argv[++i]).toULongLong();
        } else if (arg == QLatin1String("--message") && i + 1 < argc) {
            message = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QLatin1String("--view") && i + 1 < argc) {
            view_mode = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QLatin1String("--view-file") && i + 1 < argc) {
            view_file = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QLatin1String("--windowed")) {
            windowed = true;
        } else if (arg == QLatin1String("--title") && i + 1 < argc) {
            title = QString::fromLocal8Bit(argv[++i]);
        } else if (!arg.startsWith(QLatin1Char('-'))) {
            Seat seat;
            seat.ring_name = arg; // bare ring names are accepted too
            seats.push_back(seat);
        }
    }
    if (seats.isEmpty() && message.isEmpty()) {
        fprintf(stderr,
                "usage: azahar-viewer [--windowed] [--view both|top] [--view-file <path>] "
                "--seat <ring[:socket]> …\n");
        return 1;
    }
    for (auto& seat : seats) {
        seat.restart_path = restart_paths.value(seat.ring_name);
    }
    Viewer viewer(seats, message);
    viewer.setWindowTitle(title);
    if (!view_file.isEmpty()) {
        viewer.set_view_file(view_file);
    }
    // A one-run override: it opens in this layout but is not what the next
    // session resumes, which is the layout the player left it in.
    if (view_mode == QLatin1String("top")) {
        viewer.set_view(View::Top, false);
    } else if (view_mode == QLatin1String("both")) {
        viewer.set_view(View::Both, false);
    }
    if (!dump_path.isEmpty() && dump_frame > 0) {
        viewer.set_dump(dump_path, dump_frame);
    }
    if (!windowed) {
        viewer.setWindowFlag(Qt::FramelessWindowHint, true);
        viewer.setGeometry(viewer.screen()->availableGeometry());
    } else {
        viewer.resize(1280, 720);
    }
    viewer.show();
    viewer.setFocus();

    return QApplication::exec();
}
