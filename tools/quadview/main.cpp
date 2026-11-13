// azahar-quadview — adopt the split-screen Azahar windows into a single window.
//
// X11/XWayland only. The emulator instances keep doing all the rendering; this
// just reparents their windows into one grid inside a single frameless
// top-level window, so the desktop sees one application window instead of four.
//
//   ./azahar-quadview            # adopt every Azahar window it can find
//   ./azahar-quadview 0x2400013 …# adopt the given window ids, in seat order
//
// Tiles are equal-sized in a near-square grid (4 seats -> 2x2). Click a tile to
// give that seat keyboard focus.

#include <QApplication>
#include <QCursor>
#include <QDebug>
#include <QGridLayout>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

namespace {

struct Found {
    Window id;
    QString title;
    int x = 0;
    int y = 0;
};

QString window_title(Display* display, Window window) {
    QString title;
    Atom net_name = XInternAtom(display, "_NET_WM_NAME", True);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", True);
    if (net_name != None && utf8 != None) {
        Atom type = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(display, window, net_name, 0, 1024, False, utf8, &type, &format,
                               &count, &remaining, &data) == Success &&
            data) {
            title = QString::fromUtf8(reinterpret_cast<char*>(data), count);
            XFree(data);
        }
    }
    if (title.isEmpty()) {
        char* name = nullptr;
        if (XFetchName(display, window, &name) && name) {
            title = QString::fromLocal8Bit(name);
            XFree(name);
        }
    }
    return title;
}

QString window_class(Display* display, Window window) {
    XClassHint* hint = XAllocClassHint();
    QString result;
    if (hint && XGetClassHint(display, window, hint)) {
        if (hint->res_class) {
            result = QString::fromLocal8Bit(hint->res_class);
        }
        if (hint->res_name) {
            XFree(hint->res_name);
        }
        if (hint->res_class) {
            XFree(hint->res_class);
        }
    }
    if (hint) {
        XFree(hint);
    }
    return result;
}

// Windows the emulator owns, in the order the window manager lists them.
std::vector<Found> find_azahar_windows(Display* display) {
    std::vector<Found> found;
    Atom client_list = XInternAtom(display, "_NET_CLIENT_LIST", True);
    if (client_list == None) {
        return found;
    }
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display, DefaultRootWindow(display), client_list, 0, 4096, False,
                           XA_WINDOW, &type, &format, &count, &remaining, &data) != Success ||
        !data) {
        return found;
    }
    const auto* windows = reinterpret_cast<Window*>(data);
    for (unsigned long i = 0; i < count; ++i) {
        // The orchestrator also calls itself "azahar-splitscreen"; only the
        // emulator's own windows are class "Azahar".
        if (window_class(display, windows[i]).compare(QStringLiteral("Azahar"),
                                                      Qt::CaseInsensitive) != 0) {
            continue;
        }
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(display, windows[i], &attrs)) {
            continue;
        }
        // A seat that failed to boot shows a normal-sized window with an error
        // dialog; only tile real game windows.
        if (attrs.width < 320 || attrs.height < 180) {
            continue;
        }
        found.push_back({windows[i], window_title(display, windows[i]), attrs.x, attrs.y});
    }
    XFree(data);
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.y / 100 != b.y / 100) {
            return a.y < b.y;
        }
        return a.x < b.x;
    });
    return found;
}

void focus_seat(Display* display, Window window) {
    XRaiseWindow(display, window);
    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    // The client also has to believe it is active; click-to-focus events reach
    // it directly because it is a real child window after reparenting.
    XFlush(display);
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("azahar-quadview"));

    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        qCritical("quadview: cannot open the X display (X11/XWayland only)");
        return 1;
    }

    std::vector<Window> seats;
    int expected = 0;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("--expect") && i + 1 < argc) {
            expected = QString::fromLocal8Bit(argv[++i]).toInt();
            continue;
        }
        bool ok = false;
        const auto id = arg.toULongLong(&ok, 0);
        if (ok) {
            seats.push_back(static_cast<Window>(id));
        }
    }
    if (seats.empty()) {
        // Wait for the whole set: adopting a half-booted session would put error
        // dialogs and menu-bar windows into the grid.
        for (int attempt = 0; attempt < 50; ++attempt) {
            const auto found = find_azahar_windows(display);
            const bool enough = expected > 0 ? static_cast<int>(found.size()) >= expected
                                             : !found.empty();
            if (enough) {
                for (const auto& window : found) {
                    qInfo("adopting 0x%lx at %d,%d  %s", window.id, window.x, window.y,
                          qPrintable(window.title));
                    seats.push_back(window.id);
                }
                break;
            }
            usleep(200'000);
        }
        if (expected > 0 && static_cast<int>(seats.size()) > expected) {
            seats.resize(static_cast<size_t>(expected));
        }
    }
    if (expected > 0 && static_cast<int>(seats.size()) < expected) {
        qCritical("quadview: expected %d seats but found %zu — leaving the windows alone",
                  expected, seats.size());
        return 1;
    }
    if (seats.empty()) {
        qCritical("quadview: no Azahar windows found — start a session first");
        return 1;
    }

    QWidget window;
    window.setWindowTitle(QStringLiteral("Azahar Splitscreen"));
    window.setWindowFlag(Qt::FramelessWindowHint);
    auto* grid = new QGridLayout(&window);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(1);

    const int columns = seats.size() >= 3 ? 2 : static_cast<int>(seats.size());
    std::vector<QWidget*> tiles;
    for (size_t i = 0; i < seats.size(); ++i) {
        QWindow* foreign = QWindow::fromWinId(static_cast<WId>(seats[i]));
        QWidget* tile = QWidget::createWindowContainer(foreign, &window);
        tile->setFocusPolicy(Qt::StrongFocus);
        const Window id = seats[i];
        // Clicking a tile hands that seat the keyboard.
        QObject::connect(foreign, &QWindow::activeChanged, &window, [display, id, foreign] {
            if (foreign->isActive()) {
                focus_seat(display, id);
            }
        });
        grid->addWidget(tile, static_cast<int>(i) / columns, static_cast<int>(i) % columns);
        tiles.push_back(tile);
        qInfo("seat %zu -> 0x%lx", i + 1, seats[i]);
    }

    // An embedded window is a child, so the window manager never gives it the
    // keyboard. Follow the pointer instead: whichever tile it is over owns the
    // X input focus, which is what makes the seat respond to key/touch input.
    auto* follow = new QTimer(&window);
    QObject::connect(follow, &QTimer::timeout, &window, [display, tiles, seats] {
        const QPoint pointer = QCursor::pos();
        for (size_t i = 0; i < tiles.size(); ++i) {
            if (!tiles[i]->rect().contains(tiles[i]->mapFromGlobal(pointer))) {
                continue;
            }
            Window focused = None;
            int revert = 0;
            XGetInputFocus(display, &focused, &revert);
            if (focused != seats[i]) {
                focus_seat(display, seats[i]);
            }
            return;
        }
    });
    follow->start(200);

    // Killing the container destroys its children, which takes the seats'
    // windows with it. Hand them back to the root window on a clean exit so the
    // emulator instances survive (and can be re-adopted).
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [display, seats] {
        for (const Window seat : seats) {
            XReparentWindow(display, seat, DefaultRootWindow(display), 0, 0);
        }
        XFlush(display);
    });

    const QRect area = window.screen()->availableGeometry();
    window.setGeometry(area);
    window.show();
    QTimer::singleShot(0, &window, [grid, columns, seats] {
        const int rows = static_cast<int>((seats.size() + columns - 1) / columns);
        for (int r = 0; r < rows; ++r) {
            grid->setRowStretch(r, 1);
        }
        for (int c = 0; c < columns; ++c) {
            grid->setColumnStretch(c, 1);
        }
    });

    return QApplication::exec();
}
