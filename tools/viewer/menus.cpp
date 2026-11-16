#include "menus.h"

#include "viewer.h"

#include <QAction>
#include <QActionGroup>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <SDL.h>

namespace {

/// The name a player recognises for a pad button. SDL's own strings ("dpup",
/// "leftshoulder") are meant for developers, so the common buttons are named
/// here and anything else falls back to SDL.
QString button_name(int button) {
    if (button < 0) {
        return QObject::tr("Unbound");
    }
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
        return QStringLiteral("A");
    case SDL_CONTROLLER_BUTTON_B:
        return QStringLiteral("B");
    case SDL_CONTROLLER_BUTTON_X:
        return QStringLiteral("X");
    case SDL_CONTROLLER_BUTTON_Y:
        return QStringLiteral("Y");
    case SDL_CONTROLLER_BUTTON_BACK:
        return QObject::tr("Select");
    case SDL_CONTROLLER_BUTTON_GUIDE:
        return QObject::tr("Guide");
    case SDL_CONTROLLER_BUTTON_START:
        return QObject::tr("Start");
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return QObject::tr("Left stick click");
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        return QObject::tr("Right stick click");
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return QObject::tr("Left shoulder");
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return QObject::tr("Right shoulder");
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return QObject::tr("D-pad up");
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return QObject::tr("D-pad down");
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return QObject::tr("D-pad left");
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return QObject::tr("D-pad right");
    case SDL_CONTROLLER_BUTTON_MISC1:
        return QObject::tr("Misc button");
    default:
        break;
    }
    const char* name =
        SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(button));
    return name != nullptr ? QString::fromLatin1(name) : QObject::tr("Button %1").arg(button);
}

/// The mapping dialog: one row per 3DS button, a Change button that takes the
/// next pad press, and a reset. The mapping applies to every pad in the session,
/// which is what a couch of interchangeable pads wants.
class PadDialog : public QDialog {
public:
    PadDialog(Viewer& viewer, QWidget* parent) : QDialog(parent), viewer_(viewer) {
        setWindowTitle(tr("Gamepad Mapping"));
        setModal(true);
        auto* layout = new QVBoxLayout(this);
        auto* hint = new QLabel(
            tr("Select an action, choose Change, then press the button on any pad.\n"
               "Every pad in the session follows the same mapping."),
            this);
        hint->setWordWrap(true);
        layout->addWidget(hint);

        table_ = new QTableWidget(0, 2, this);
        table_->setHorizontalHeaderLabels({tr("Action"), tr("Pad button")});
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table_->verticalHeader()->hide();
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        layout->addWidget(table_);

        status_ = new QLabel(this);
        status_->setStyleSheet(QStringLiteral("color: #8b93a7;"));
        layout->addWidget(status_);

        auto* buttons = new QHBoxLayout();
        auto* change = new QPushButton(tr("Change…"), this);
        auto* reset = new QPushButton(tr("Reset to Defaults"), this);
        auto* close = new QPushButton(tr("Close"), this);
        buttons->addWidget(change);
        buttons->addWidget(reset);
        buttons->addStretch();
        buttons->addWidget(close);
        layout->addLayout(buttons);

        connect(change, &QPushButton::clicked, this, [this] { listen(); });
        connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) { listen(); });
        connect(reset, &QPushButton::clicked, this, [this] {
            viewer_.cancel_pad_listen();
            viewer_.reset_pad_actions();
            status_->setText(tr("Defaults restored."));
            fill();
        });
        connect(close, &QPushButton::clicked, this, [this] { accept(); });
        resize(460, 420);
        fill();
    }

    // Closing the dialog must not leave the next pad press swallowed by a
    // listener nobody is waiting for.
    void accept() override {
        viewer_.cancel_pad_listen();
        QDialog::accept();
    }

    void reject() override {
        viewer_.cancel_pad_listen();
        QDialog::reject();
    }

private:
    void fill() {
        const auto actions = viewer_.pad_actions();
        table_->setRowCount(static_cast<int>(actions.size()));
        for (int row = 0; row < actions.size(); ++row) {
            table_->setItem(row, 0, new QTableWidgetItem(actions[row].name));
            table_->setItem(row, 1, new QTableWidgetItem(button_name(actions[row].button)));
        }
        if (table_->currentRow() < 0 && !actions.isEmpty()) {
            table_->selectRow(0);
        }
    }

    void listen() {
        const int row = table_->currentRow();
        if (row < 0) {
            status_->setText(tr("Pick an action first."));
            return;
        }
        status_->setText(tr("Press a button on a pad…"));
        viewer_.listen_for_pad_button([this, row](int button) {
            viewer_.set_pad_action(row, button);
            fill();
            table_->selectRow(row);
            status_->setText(tr("Saved."));
        });
    }

    Viewer& viewer_;
    QTableWidget* table_ = nullptr;
    QLabel* status_ = nullptr;
};

QString controls_html(const Viewer& viewer) {
    QString html = QStringLiteral(
        "<b>Keyboard and mouse</b><ul>"
        "<li><b>Tab</b> — both screens / top screen only (also the View menu)</li>"
        "<li><b>F5–F10</b> — hold to peek at that player's touch screen</li>"
        "<li><b>Click</b> — select a player; a click on their screen is a touch</li>"
        "<li><b>F11</b> — fullscreen, <b>Esc</b> — quit the session</li>"
        "</ul><b>Gamepad</b> (every pad uses the same mapping)<ul>"
        "<li><b>Left stick</b> — circle pad</li>"
        "<li><b>Right stick</b> — move the touch pointer, <b>click it</b> to tap</li>"
        "<li><b>Hold left shoulder</b> — peek at your touch screen</li>");
    for (const auto& action : viewer.pad_actions()) {
        if (action.button < 0) {
            continue;
        }
        // Pad first, console button second: the labels often match, and
        // "Pad A -> A" still reads correctly where "A - A" does not.
        html += QStringLiteral("<li>Pad <b>%1</b> → %2</li>")
                    .arg(button_name(action.button).toHtmlEscaped(), action.name.toHtmlEscaped());
    }
    html += QStringLiteral("</ul>");
    return html;
}

} // namespace

void attach_menus(QMainWindow& window, Viewer& viewer) {
    QMenuBar* bar = window.menuBar();

    QMenu* session = bar->addMenu(QObject::tr("&Session"));
    QAction* quit = session->addAction(QObject::tr("&Quit Session"));
    QObject::connect(quit, &QAction::triggered, &window, &QMainWindow::close);

    // The layout the window opens with is the session's; the check marks follow
    // Tab as well as a click, so the menu always says what is on screen.
    QMenu* view = bar->addMenu(QObject::tr("&View"));
    QAction* both = view->addAction(QObject::tr("&Both Screens"));
    QAction* top = view->addAction(QObject::tr("&Top Screen Only"));
    auto* group = new QActionGroup(&window);
    for (QAction* action : {both, top}) {
        action->setCheckable(true);
        group->addAction(action);
    }
    both->setChecked(viewer.view() == View::Both);
    top->setChecked(viewer.view() == View::Top);
    QObject::connect(both, &QAction::triggered, &viewer, [&viewer] { viewer.set_view(View::Both); });
    QObject::connect(top, &QAction::triggered, &viewer, [&viewer] { viewer.set_view(View::Top); });
    QObject::connect(view, &QMenu::aboutToShow, &window, [&viewer, both, top] {
        (viewer.view() == View::Both ? both : top)->setChecked(true);
    });

    view->addSeparator();
    QAction* fullscreen = view->addAction(QObject::tr("&Fullscreen"));
    fullscreen->setCheckable(true);
    fullscreen->setChecked(window.isFullScreen());
    fullscreen->setShortcut(QKeySequence(Qt::Key_F11));
    QObject::connect(fullscreen, &QAction::triggered, &window, [&window, fullscreen] {
        if (window.isFullScreen()) {
            window.showNormal();
        } else {
            window.showFullScreen();
        }
        fullscreen->setChecked(window.isFullScreen());
    });
    QObject::connect(view, &QMenu::aboutToShow, &window, [&window, fullscreen] {
        fullscreen->setChecked(window.isFullScreen());
    });

    QMenu* controls = bar->addMenu(QObject::tr("&Controls"));
    QAction* mapping = controls->addAction(QObject::tr("&Gamepad Mapping…"));
    QObject::connect(mapping, &QAction::triggered, &window, [&window, &viewer] {
        PadDialog dialog(viewer, &window);
        dialog.exec();
    });
    QAction* reference = controls->addAction(QObject::tr("Controls &Reference"));
    reference->setShortcut(QKeySequence(Qt::Key_F1));
    QObject::connect(reference, &QAction::triggered, &window, [&window, &viewer] {
        QMessageBox box(&window);
        box.setWindowTitle(QObject::tr("Controls"));
        box.setTextFormat(Qt::RichText);
        box.setText(controls_html(viewer));
        box.exec();
    });

    QMenu* help = bar->addMenu(QObject::tr("&Help"));
    QAction* about = help->addAction(QObject::tr("&About Azahar Splitscreen"));
    QObject::connect(about, &QAction::triggered, &window, [&window] {
        QMessageBox::about(
            &window, QObject::tr("About Azahar Splitscreen"),
            QObject::tr("<b>Azahar Splitscreen</b><br/>"
                        "2–6 Nintendo 3DS consoles in one window.<br/><br/>"
                        "Every player runs their own Azahar instance into a shared-memory "
                        "frame ring; this window composites them and forwards input."));
    });
}
