// azahar-viewer — the single window of a split-screen session.
//
// One window, one GL context, one tile per seat. Each seat's emulator process
// renders into a shared-memory frame ring; this reads the newest complete frame
// from each ring and draws it. It never blocks on a seat: a tile whose ring has
// not advanced simply keeps its previous frame.
//
// Input goes the other way: keys and touch clicks are forwarded as datagrams to
// the selected seat's socket, so the emulator instances stay headless.

#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QTimer>
#include <QVector>

#include <vector>

/// One seat: its frame ring plus the socket its input goes to.
struct Seat {
    QString ring_name;
    QString socket_path;

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

    QImage canvas;    // newest frame, top-down BGRA (Format_ARGB32 on LE)
    unsigned tex = 0; // GL texture for `canvas`
    bool uploaded = false;
};

class Viewer : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit Viewer(QVector<Seat> seats, QWidget* parent = nullptr);

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
    void initializeGL() override;
    void paintGL() override;

    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void open_ring(Seat& seat);
    bool grab_frame(Seat& seat);
    /// Tile rectangle (in this widget's pixels) each seat is drawn into.
    QRect tile_rect(int index) const;
    void send_input(int seat, quint8 type, quint32 key, quint16 x, quint16 y);
    void log_stats();
    void dump_frame();

    QVector<Seat> seats_;
    int selected_ = 0;
    bool dragging_ = false;

    QOpenGLShaderProgram program_;
    QOpenGLVertexArrayObject vao_;
    QOpenGLBuffer quad_{QOpenGLBuffer::VertexBuffer};
    QTimer frame_timer_;
    QElapsedTimer clock_;

    quint64 presented_ = 0;
    quint64 paint_ns_ = 0;
    quint64 ticks_ = 0;
    QString dump_path_;
    quint64 dump_frame_ = 0;
    qint64 last_stats_ms_ = 0;
};
