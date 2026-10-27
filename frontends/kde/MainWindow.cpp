/*
 * MainWindow -- see MainWindow.h.
 *
 * The PC's picture, the Ctrl+Alt+Del and Reset buttons, the menus, the
 * keyboard and mouse, the drives, the FujiNet status, and the timers that
 * bring the other threads' news to the UI. With no BIOS imported yet it
 * shows how to import one instead.
 *
 * Keys go to the PC pressed and released, by physical key: the PC keyboard
 * is positional, F1-F12 and the Control and Alt combinations included. The
 * app's own shortcuts are Ctrl+Shift ones (application-wide, so they work
 * from the Controllers panel and the debugger), plus Ctrl+End (let the mouse
 * go) and Ctrl+Alt+PgDn (fullscreen), as in 86Box. The menu bar has no Alt
 * mnemonics and no Alt-key navigation: Alt is the PC's.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProxyStyle>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <initializer_list>

#include "DisplayWidget.h"
#include "FujiNetWindows.h"
#include "KeyForward.h"
#include "SettingsDialog.h"
#include "controllers/ControllersWindow.h"
#include "debugger/DebuggerWindow.h"
#include "help/HelpWindow.h"

namespace {

QString local(const char *s) { return QString::fromUtf8(s ? s : ""); }

/* "Floppy images (*.img *.IMG ...);;All files (*)": either case, as GNOME's
 * suffix filters. */
QString filter(const char *name, std::initializer_list<const char *> exts)
{
    QStringList pats;
    for (const char *e : exts)
        pats << QStringLiteral("*.%1").arg(QLatin1String(e)) << QStringLiteral("*.%1").arg(QLatin1String(e).toString().toUpper());
    return QStringLiteral("%1 (%2);;All files (*)").arg(QLatin1String(name), pats.join(QLatin1Char(' ')));
}

const auto kFloppyExts = { "img", "ima", "vfd", "flp", "dsk", "86f", "imd", "td0", "fdi" };
const auto kHdExts = { "vhd", "img", "hdi", "hdx" };
const auto kAnyExts = { "img", "ima", "vfd", "flp", "dsk", "86f", "imd", "td0", "fdi", "vhd", "hdi", "hdx",
                        "zip", "rom", "bin", "exe", "com" };
const auto kRomExts = { "zip", "rom", "bin", "awd", "bio", "vbi", "raw" };

/* A file name in a menu keeps its own & */
QString menuEscape(QString s) { return s.replace(QLatin1Char('&'), QLatin1String("&&")); }

bool envOn(const char *name)
{
    const QByteArray v = qgetenv(name);
    return !v.isEmpty() && v[0] != '0';
}

/* No Alt-key navigation: a lone Alt would otherwise take the keyboard into
 * the menu bar, away from the PC. */
class NoAltMenuStyle : public QProxyStyle {
public:
    int styleHint(StyleHint hint, const QStyleOption *opt, const QWidget *w, QStyleHintReturn *ret) const override
    {
        if (hint == SH_MenuBar_AltKeyNavigation)
            return 0;
        return QProxyStyle::styleHint(hint, opt, w, ret);
    }
};

/* A status line that elides instead of widening the window. */
class ElidedLabel : public QLabel {
public:
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = contentsRect();
        p.setPen(palette().color(foregroundRole()));
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideRight, r.width()));
    }
};

QAction *appShortcut(QWidget *owner, QAction *a, const QList<QKeySequence> &keys)
{
    /* application-wide: the panels have the keyboard too */
    a->setShortcuts(keys);
    a->setShortcutContext(Qt::ApplicationShortcut);
    owner->addAction(a);
    return a;
}

} // namespace

MainWindow::MainWindow(msdossession *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go MS-DOS"));
    /* 640 x 480 at one and a half */
    resize(960, 720 + 70);
    setAcceptDrops(true);
    setContextMenuPolicy(Qt::NoContextMenu);

    m_display = new DisplayWidget(session, this);
    connect(m_display, &DisplayWidget::captureChanged, this, [this](bool on) {
        if (on) toast(QStringLiteral("Mouse captured: Ctrl+End or the middle button lets it go"));
    });
    m_noBios = buildNoBiosPage();
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_display);
    m_stack->addWidget(m_noBios);
    setCentralWidget(m_stack);
    m_display->setFocus();

    /* the toolbar: the machine's buttons, then the status */
    QToolBar *tb = addToolBar(QStringLiteral("Machine"));
    tb->setMovable(false);
    tb->setFloatable(false);
    m_cadButton = new QPushButton(QStringLiteral("Ctrl+Alt+Del"));
    m_cadButton->setToolTip(QStringLiteral("Send Ctrl+Alt+Del: DOS restarts (a warm boot)"));
    m_cadButton->setFocusPolicy(Qt::NoFocus);
    connect(m_cadButton, &QPushButton::clicked, this, [this] { msdossession_ctrl_alt_del(m_session); });
    m_resetButton = new QPushButton(QStringLiteral("Reset"));
    m_resetButton->setToolTip(QStringLiteral("Press the reset button: the PC starts again from cold (Ctrl+Shift+R)"));
    m_resetButton->setFocusPolicy(Qt::NoFocus);
    tb->addWidget(m_cadButton);
    tb->addWidget(m_resetButton);

    auto *statusBox = new QWidget;
    auto *sh = new QHBoxLayout(statusBox);
    sh->setContentsMargins(10, 0, 6, 0);
    sh->setSpacing(8);
    m_dot = new QLabel;
    m_dot->setFixedSize(12, 12);
    m_dot->setToolTip(QStringLiteral("FujiNet link"));
    m_status = new ElidedLabel;
    m_status->setText(QStringLiteral("Starting…"));
    m_status->setStyleSheet(QStringLiteral("color: gray;"));
    m_status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sh->addWidget(m_dot);
    sh->addWidget(m_status, 1);
    QFont mono = font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    mono.setPointSizeF((mono.pointSizeF() > 0 ? mono.pointSizeF() : 10) * 0.85);
    static const char *const names[4] = { "A:", "B:", "C:", "D:" };
    for (int i = 0; i < 4; ++i) {
        m_lights[i] = new QLabel(QString::fromLatin1(names[i]));
        m_lights[i]->setFont(mono);
        m_lights[i]->setToolTip(QStringLiteral("Drive activity"));
        m_lights[i]->setVisible(false);
        sh->addWidget(m_lights[i]);
    }
    statusBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(statusBox);

    m_toast = new QLabel(this);
    m_toast->setWordWrap(true);
    m_toast->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_toast->setAlignment(Qt::AlignCenter);
    m_toast->setStyleSheet(QStringLiteral(
        "background: rgba(30, 30, 30, 235); color: white; border-radius: 8px; padding: 8px 14px;"));
    m_toast->hide();
    m_toastTimer.setSingleShot(true);
    connect(&m_toastTimer, &QTimer::timeout, m_toast, &QWidget::hide);

    buildMenus();
    applyView();

    connect(&m_statusTimer, &QTimer::timeout, this, [this] {
        updateStatus();
        updateActions();
    });
    m_statusTimer.start(1000);
    connect(&m_fastTimer, &QTimer::timeout, this, &MainWindow::fastTick);
    m_fastTimer.start(100);
    /* the current pads and FujiNet state are the baseline: no toasts for them */
    m_padGeneration = msdossession_gamepad_generation(session);
    m_fujinetGeneration = msdossession_fujinet_generation(session);
    updateActions();
    updateStatus();
    showPage();

    /* MSDOS_OPEN_CONTROLLERS / _DEBUGGER / _SETTINGS open those windows at
     * launch, MSDOS_OPEN_HELP=<slug, context or ?words> the Help ("1": its
     * start). MSDOS_SCREENSHOT=file.png saves the picture after
     * MSDOS_SCREENSHOT_FRAMES frames (300); MSDOS_AUTOQUIT_FRAMES quits after
     * that many. */
    if (envOn("MSDOS_OPEN_CONTROLLERS"))
        QTimer::singleShot(0, this, [this] { toggleControllers(); });
    if (envOn("MSDOS_OPEN_DEBUGGER"))
        QTimer::singleShot(0, this, [this] { msdosDebuggerShow(this, m_session); });
    if (envOn("MSDOS_OPEN_SETTINGS"))
        QTimer::singleShot(0, this, &MainWindow::showSettings);
    if (envOn("MSDOS_OPEN_HELP"))
        QTimer::singleShot(0, this, [this] {
            const QString id = qEnvironmentVariable("MSDOS_OPEN_HELP");
            msdosHelpShow(this, id == QLatin1String("1") ? QString() : id);
        });
    m_shotPath = qEnvironmentVariable("MSDOS_SCREENSHOT");
    m_shotFrames = qEnvironmentVariableIsSet("MSDOS_SCREENSHOT_FRAMES")
                       ? qEnvironmentVariable("MSDOS_SCREENSHOT_FRAMES").toULongLong() : 300;
    m_quitFrames = qEnvironmentVariable("MSDOS_AUTOQUIT_FRAMES").toULongLong();
}

/* ---- toasts -------------------------------------------------------------------- */

void MainWindow::toast(const QString &text)
{
    m_toast->setText(text);
    placeToast();
    m_toast->show();
    m_toast->raise();
    m_toastTimer.start(text.size() > 80 ? 6000 : 3000);
}

void MainWindow::placeToast()
{
    const QRect c = m_stack->geometry();
    /* one line if it fits, wrapped within the picture otherwise */
    const QMargins pad = m_toast->contentsMargins();
    const int w = qMin(m_toast->fontMetrics().horizontalAdvance(m_toast->text()) + pad.left() + pad.right() + 30,
                       qMin(560, c.width() - 32));
    m_toast->setFixedSize(w, m_toast->heightForWidth(w));
    m_toast->move(c.x() + (c.width() - m_toast->width()) / 2, c.bottom() - m_toast->height() - 16);
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    if (m_toast->isVisible()) placeToast();
}

/* ---- menus ---------------------------------------------------------------------- */

void MainWindow::buildMenus()
{
    auto *mbStyle = new NoAltMenuStyle;
    mbStyle->setParent(menuBar());
    menuBar()->setStyle(mbStyle);

    QMenu *machine = menuBar()->addMenu(QStringLiteral("Machine"));
    appShortcut(this, machine->addAction(QStringLiteral("&Open…"), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Open"), mediaFolder(),
                                                       filter("Disk images and programs", kAnyExts));
        if (!f.isEmpty())
            loadMedia(f);
    }), { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O) });
    machine->addSeparator();
    m_cadAction = machine->addAction(QStringLiteral("Ctrl+&Alt+Del"), this,
                                     [this] { msdossession_ctrl_alt_del(m_session); });
    QAction *reset = appShortcut(this, machine->addAction(QStringLiteral("&Reset"), this, [this] {
        if (!msdossession_is_running(m_session)) {
            start();
            return;
        }
        msdossession_reset(m_session);
        updateStatus();
    }), { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R) });
    connect(m_resetButton, &QPushButton::clicked, reset, &QAction::trigger);
    m_pauseAction = machine->addAction(QStringLiteral("&Pause"));
    m_pauseAction->setCheckable(true);
    connect(m_pauseAction, &QAction::triggered, this, [this](bool on) {
        msdossession_set_paused(m_session, on);
        updateStatus();
    });
    m_warpAction = appShortcut(this, machine->addAction(QStringLiteral("&Warp Speed")),
                               { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_W) });
    m_warpAction->setCheckable(true);
    connect(m_warpAction, &QAction::triggered, this, [this](bool on) {
        msdossession_set_warp(m_session, on);
        updateStatus();
    });
    machine->addSeparator();
    machine->addAction(QStringLiteral("Import &BIOS…"), this, [this] { importBios(false); });
    machine->addAction(QStringLiteral("Import BIOS from a &Folder…"), this, [this] { importBios(true); });
    machine->addSeparator();
    appShortcut(this, machine->addAction(QStringLiteral("P&references…"), this, &MainWindow::showSettings),
                { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P) });
    machine->addSeparator();
    appShortcut(this, machine->addAction(QStringLiteral("&Quit"), this, [this] { close(); }),
                { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Q) });
    connect(machine, &QMenu::aboutToShow, this, [this] {
        m_pauseAction->setChecked(msdossession_paused(m_session));
        m_warpAction->setChecked(msdossession_warp(m_session));
        updateActions();
    });

    m_drives = menuBar()->addMenu(QStringLiteral("Drives"));
    connect(m_drives, &QMenu::aboutToShow, this, &MainWindow::fillDrivesMenu);
    fillDrivesMenu();

    QMenu *edit = menuBar()->addMenu(QStringLiteral("Edit"));
    m_copyAction = appShortcut(this, edit->addAction(QStringLiteral("&Copy Screen Text"), this, [this] {
        static char buf[16384];
        if (msdossession_copy_screen_text(m_session, buf, sizeof buf) <= 0) {
            toast(QStringLiteral("No text screen to copy"));
            return;
        }
        QApplication::clipboard()->setText(QString::fromUtf8(buf));
        toast(QStringLiteral("The screen's text is on the clipboard"));
    }), { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C) });
    m_pasteAction = appShortcut(this, edit->addAction(QStringLiteral("P&aste"), this, [this] {
        const QString t = QApplication::clipboard()->text();
        if (!t.isEmpty())
            msdossession_paste(m_session, t.toUtf8().constData());
    }), { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V) });

    QMenu *view = menuBar()->addMenu(QStringLiteral("View"));
    appShortcut(this, view->addAction(QStringLiteral("C&ontrollers"), this, [this] { toggleControllers(); }),
                { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_K) });
    appShortcut(this, view->addAction(QStringLiteral("De&bugger"), this,
                                      [this] { msdosDebuggerToggle(this, m_session); }),
                { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D) });
    view->addSeparator();
    m_aspectAction = view->addAction(QStringLiteral("&4:3 Picture"));
    m_aspectAction->setCheckable(true);
    connect(m_aspectAction, &QAction::triggered, this, [this](bool on) { togglePref("aspect43", on); });
    m_integerAction = view->addAction(QStringLiteral("&Whole-Number Scaling"));
    m_integerAction->setCheckable(true);
    connect(m_integerAction, &QAction::triggered, this, [this](bool on) { togglePref("integer_scale", on); });
    m_smoothAction = view->addAction(QStringLiteral("S&mooth Scaling"));
    m_smoothAction->setCheckable(true);
    connect(m_smoothAction, &QAction::triggered, this, [this](bool on) { togglePref("smooth", on); });
    view->addSeparator();
    appShortcut(this, view->addAction(QStringLiteral("&Fullscreen (Ctrl+Alt+PgDn)"), this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    }), { QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F) });

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("FujiNet"));
    m_webUiAction = fuji->addAction(QStringLiteral("FujiNet &Web UI"), this, [this] {
        if (!msdossession_fujinet_running(m_session)) {
            toast(QStringLiteral("FujiNet is off (Preferences ▸ FujiNet)"));
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] { fujinet_log_show(this, m_session); });
    m_restartAction = fuji->addAction(QStringLiteral("R&estart FujiNet"), this, [this] {
        if (msdossession_fujinet_restart(m_session) != 0)
            QMessageBox::warning(this, QStringLiteral("Cannot Restart FujiNet"), local(msdossession_last_error(m_session)));
        else
            toast(QStringLiteral("FujiNet restarted"));
    });
    connect(fuji, &QMenu::aboutToShow, this, &MainWindow::updateActions);

    QMenu *help = menuBar()->addMenu(QStringLiteral("Help"));
    connect(appShortcut(this, help->addAction(QStringLiteral("FujiNet Go MS-DOS &Help")),
                        { QKeySequence(QStringLiteral("Ctrl+Shift+H")) }),
            &QAction::triggered, this, [this] {
        msdosHelpFor(QApplication::activeWindow() ? QApplication::activeWindow() : this);
    });
    help->addSeparator();
    help->addAction(QStringLiteral("&About FujiNet Go MS-DOS"), this, &MainWindow::showAbout);
}

/* Rebuilt each time it opens: the drives follow the machine. */
void MainWindow::fillDrivesMenu()
{
    for (QMenu *sub : m_drives->findChildren<QMenu *>(Qt::FindDirectChildrenOnly))
        sub->deleteLater();
    m_drives->clear();
    m_drives->addSection(QStringLiteral("The PC's drives"));
    for (int d = 0; d < MSDOS_FLOPPIES; ++d) {
        msdos_floppy_info fi;
        if (msdossession_floppy_info(m_session, d, &fi) != 0 || !fi.present)
            continue;
        QMenu *sub = m_drives->addMenu(QStringLiteral("%1 %2").arg(d ? QStringLiteral("&B:") : QStringLiteral("&A:"),
                                                                  menuEscape(local(fi.type))));
        const QString name = fi.boot_disk ? QStringLiteral("FujiNet boot floppy")
                             : fi.path[0] ? QFileInfo(local(fi.path)).fileName() : QStringLiteral("Empty");
        sub->addAction(menuEscape(name) + (fi.read_only ? QStringLiteral(" (write-protected)") : QString()))
            ->setEnabled(false);
        sub->addSeparator();
        sub->addAction(QStringLiteral("&Insert…"), this, [this, d] {
            const QString f = QFileDialog::getOpenFileName(this, d ? QStringLiteral("Insert in B:") : QStringLiteral("Insert in A:"),
                                                           mediaFolder(), filter("Floppy images", kFloppyExts));
            if (f.isEmpty())
                return;
            char msg[1024];
            if (msdossession_floppy_insert(m_session, d, f.toLocal8Bit().constData(), 0, msg, sizeof msg) == 0) {
                rememberFolder(f);
                toast(local(msg));
            } else {
                QMessageBox::warning(this, QStringLiteral("Cannot Insert the Disk"), local(msg));
            }
        });
        sub->addAction(QStringLiteral("&Eject"), this, [this, d] { msdossession_floppy_eject(m_session, d); });
        const bool ro = fi.read_only;
        sub->addAction(ro ? QStringLiteral("Allow &Writing") : QStringLiteral("&Write-Protect"), this,
                       [this, d, ro] { msdossession_floppy_set_read_only(m_session, d, !ro); });
        sub->addAction(QStringLiteral("FujiNet &Boot Floppy"), this, [this, d] {
            msdossession_floppy_boot_disk(m_session, d);
            toast(QStringLiteral("The FujiNet boot floppy is in the drive"));
        });
        sub->addAction(QStringLiteral("&New Blank Disk…"), this, [this, d] {
            const QString f = QFileDialog::getSaveFileName(this, QStringLiteral("New Blank Floppy"),
                                                           mediaFolder() + QStringLiteral("/blank.img"));
            if (f.isEmpty())
                return;
            char msg[512], ins[512];
            const QByteArray p = f.toLocal8Bit();
            if (msdossession_new_blank_floppy(m_session, d, p.constData(), msg, sizeof msg) == 0
                && msdossession_floppy_insert(m_session, d, p.constData(), 0, ins, sizeof ins) == 0)
                toast(local(ins));
            else
                QMessageBox::warning(this, QStringLiteral("Cannot Make the Disk"), local(msg));
        });
    }
    for (int d = 0; d < MSDOS_HARD_DISKS; ++d) {
        msdos_hd_info hi;
        if (msdossession_hd_info(m_session, d, &hi) != 0 || !hi.present)
            continue;
        QMenu *sub = m_drives->addMenu(d ? QStringLiteral("&D: hard disk") : QStringLiteral("&C: hard disk"));
        sub->addAction(menuEscape(hi.path[0] ? QFileInfo(local(hi.path)).fileName() : QStringLiteral("None")))
            ->setEnabled(false);
        sub->addSeparator();
        sub->addAction(QStringLiteral("&Attach…"), this, [this, d] {
            const QString f = QFileDialog::getOpenFileName(this, d ? QStringLiteral("Attach as D:") : QStringLiteral("Attach as C:"),
                                                           mediaFolder(), filter("Hard disk images", kHdExts));
            if (!f.isEmpty())
                attachHd(d, f);
        });
        sub->addAction(QStringLiteral("&Detach"), this, [this, d] {
            msdossession_hd_detach(m_session, d);
            toast(QStringLiteral("Detached from the next reset"));
        });
        sub->addAction(QStringLiteral("&New Blank Disk…"), this, [this, d] {
            const QString f = QFileDialog::getSaveFileName(
                this, QStringLiteral("New Blank Hard Disk"),
                mediaFolder() + (d ? QStringLiteral("/drive-d.vhd") : QStringLiteral("/drive-c.vhd")));
            if (f.isEmpty())
                return;
            char msg[512];
            if (msdossession_new_blank_hd(m_session, d, f.toLocal8Bit().constData(), msg, sizeof msg) == 0) {
                attachHd(d, f);
                QMessageBox::information(this, QStringLiteral("New Hard Disk"), local(msg));
            } else {
                QMessageBox::warning(this, QStringLiteral("Cannot Make the Disk"), local(msg));
            }
        });
    }
    m_drives->addSection(QStringLiteral("FujiNet's SD card"));
    m_drives->addAction(QStringLiteral("&Import to SD Card…"), this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Import to FujiNet's SD Card"),
                                                                mediaFolder(), filter("Disk images and programs", kAnyExts));
        if (files.isEmpty())
            return;
        int n = 0;
        for (const QString &f : files) {
            char dest[1024];
            if (msdossession_import_to_sd(m_session, f.toLocal8Bit().constData(), dest, sizeof dest) == 0)
                n++;
        }
        if (n)
            toast(QStringLiteral("%1 file%2 on FujiNet's SD card: mount %3 in CONFIG")
                      .arg(n).arg(n == 1 ? QStringLiteral(" is") : QStringLiteral("s are"),
                                  n == 1 ? QStringLiteral("it") : QStringLiteral("them")));
        else
            QMessageBox::warning(this, QStringLiteral("Nothing Imported"), QStringLiteral("The files could not be copied."));
    });
    m_drives->addAction(QStringLiteral("Open &SD Folder"), this, [this] {
        const QString dir = local(msdossession_sd_path(m_session));
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
}

/* What can be used now: most of it needs the PC running. */
void MainWindow::updateActions()
{
    const bool up = msdossession_is_running(m_session) != 0;
    m_webUiAction->setEnabled(msdossession_fujinet_running(m_session));
    m_restartAction->setEnabled(up && msdossession_get_int(m_session, "fujinet", 1));
    m_cadAction->setEnabled(up);
    m_pauseAction->setEnabled(up);
    m_warpAction->setEnabled(up);
    m_copyAction->setEnabled(up);
    m_pasteAction->setEnabled(up);
}

/* ---- media --------------------------------------------------------------------- */

QString MainWindow::mediaFolder() const
{
    const QString last = local(msdossession_get_str(m_session, "last_dir", ""));
    if (!last.isEmpty() && QFileInfo(last).isDir())
        return last;
    return local(msdossession_media_path(m_session));
}

void MainWindow::rememberFolder(const QString &path)
{
    msdossession_set_str(m_session, "last_dir", QFileInfo(path).absolutePath().toLocal8Bit().constData());
}

void MainWindow::attachHd(int drive, const QString &path)
{
    char msg[1024];
    if (msdossession_hd_attach(m_session, drive, path.toLocal8Bit().constData(), msg, sizeof msg) != 0) {
        QMessageBox::warning(this, QStringLiteral("Cannot Attach the Disk"), local(msg));
        return;
    }
    rememberFolder(path);
    toast(local(msg));
}

void MainWindow::askHdDrive(const QString &path)
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Which Drive?"));
    box.setText(QStringLiteral("<b>Which Drive?</b>"));
    box.setInformativeText(QStringLiteral("%1 is a hard disk image. It takes effect from the next reset.")
                               .arg(QFileInfo(path).fileName()));
    QPushButton *c = box.addButton(QStringLiteral("C:"), QMessageBox::AcceptRole);
    QPushButton *d = box.addButton(QStringLiteral("D:"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(c);
    box.exec();
    if (box.clickedButton() == c)
        attachHd(0, path);
    else if (box.clickedButton() == d)
        attachHd(1, path);
}

void MainWindow::loadMedia(const QString &path)
{
    char msg[2048];
    const int rc = msdossession_open(m_session, path.toLocal8Bit().constData(), msg, sizeof msg);
    if (rc == 1) {
        askHdDrive(path);
        return;
    }
    if (rc != 0) {
        QMessageBox::warning(this, QStringLiteral("Cannot Open It"),
                             msg[0] ? local(msg) : local(msdossession_last_error(m_session)));
        return;
    }
    rememberFolder(path);
    toast(msg[0] ? local(msg) : QFileInfo(path).fileName());
    settingsChanged();
    start();
}

void MainWindow::importBios(bool folder)
{
    QStringList paths;
    if (folder) {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Import BIOS from a Folder"));
        if (!dir.isEmpty())
            paths << dir;
    } else {
        paths = QFileDialog::getOpenFileNames(this, QStringLiteral("Import BIOS"), QString(),
                                              filter("BIOS images and ROM sets", kRomExts));
    }
    QStringList log;
    int imported = 0;
    for (const QString &p : paths) {
        char msg[2048];
        if (msdossession_rom_import(m_session, p.toLocal8Bit().constData(), msg, sizeof msg) == 0)
            imported++;
        log << local(msg);
    }
    if (!log.isEmpty())
        QMessageBox::information(this, imported ? QStringLiteral("BIOS Imported") : QStringLiteral("Nothing Imported"),
                                 log.join(QLatin1Char('\n')));
    settingsChanged();
    start();
}

/* ---- the machine ------------------------------------------------------------------ */

QWidget *MainWindow::buildNoBiosPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->addStretch(1);
    auto *title = new QLabel(QStringLiteral("No PC BIOS"));
    QFont tf = title->font();
    tf.setPointSizeF((tf.pointSizeF() > 0 ? tf.pointSizeF() : 10) * 1.8);
    tf.setBold(true);
    title->setFont(tf);
    title->setAlignment(Qt::AlignCenter);
    auto *body = new QLabel(QStringLiteral(
        "IBM's and the clone makers' BIOS ROMs are not part of the app. Import the ones you have -- "
        "single images, a zip, or a folder such as an 86Box ROM set -- and the PC starts."));
    body->setWordWrap(true);
    body->setAlignment(Qt::AlignCenter);
    body->setFixedWidth(520);
    body->setMinimumHeight(body->heightForWidth(520));
    auto *row = new QHBoxLayout;
    auto *files = new QPushButton(QStringLiteral("Import a BIOS…"));
    auto *folder = new QPushButton(QStringLiteral("Import a Folder…"));
    auto *help = new QPushButton(QStringLiteral("Help"));
    files->setDefault(true);
    connect(files, &QPushButton::clicked, this, [this] { importBios(false); });
    connect(folder, &QPushButton::clicked, this, [this] { importBios(true); });
    connect(help, &QPushButton::clicked, this, [this] { msdosHelpShow(this, QStringLiteral("nobios")); });
    row->addStretch(1);
    row->addWidget(files);
    row->addWidget(folder);
    row->addWidget(help);
    row->addStretch(1);
    v->addWidget(title);
    v->addSpacing(8);
    v->addWidget(body, 0, Qt::AlignHCenter);
    v->addSpacing(16);
    v->addLayout(row);
    auto *roms = new QLabel(QStringLiteral("<a href=\"%1\">Get the 86Box ROM set</a>")
                                .arg(QStringLiteral(MSDOSSESSION_ROMS_URL)));
    roms->setOpenExternalLinks(true);
    roms->setAlignment(Qt::AlignCenter);
    v->addSpacing(8);
    v->addWidget(roms);
    v->addStretch(1);
    return page;
}

void MainWindow::showPage()
{
    const bool bios = msdossession_has_bios(m_session) != 0;
    const bool up = msdossession_is_running(m_session) != 0;
    m_stack->setCurrentWidget(bios ? static_cast<QWidget *>(m_display) : m_noBios);
    setProperty("msdosHelpContext", bios ? QStringLiteral("main") : QStringLiteral("nobios"));
    m_cadButton->setEnabled(up);
    m_resetButton->setEnabled(up);
}

void MainWindow::start()
{
    showPage();
    if (msdossession_is_running(m_session) || !msdossession_has_bios(m_session))
        return;
    msdossession_start_opts opts;
    msdossession_default_opts(m_session, &opts);
    if (msdossession_start(m_session, &opts) != 0) {
        QMessageBox::warning(this, QStringLiteral("Cannot Start the PC"), local(msdossession_last_error(m_session)));
        updateStatus();
        return;
    }
    updateActions();
    updateStatus();
    showPage();
    m_display->setFocus();
}

void MainWindow::showSettings()
{
    SettingsDialog::Hooks hooks;
    hooks.live = [this] { applyView(); };
    hooks.imported = [this] { settingsChanged(); };
    const int did = SettingsDialog::run(this, m_session, hooks);
    settingsChanged();
    if (did == MSDOS_APPLY_RESET)
        toast(QStringLiteral("Reset with the new configuration"));
    else if (did == MSDOS_APPLY_RESTART)
        toast(QStringLiteral("FujiNet restarted"));
}

void MainWindow::toggleControllers()
{
    msdosControllersToggle(this, m_session, [this](QKeyEvent *e, bool down) { return handleKey(e, down); });
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("About FujiNet Go MS-DOS"),
        QStringLiteral("<b>FujiNet Go MS-DOS</b> %1<br><br>"
                       "IBM PCs and compatibles, from the 5150 to a Pentium, with a FujiNet on a COM port. "
                       "The emulator is 86Box (GPL-2.0-or-later, the 86Box team), with the FujiNet "
                       "firmware (GPL-3.0-or-later) talking to FUJINET.SYS. The boot floppy is FreeDOS "
                       "(GPL). The BIOS ROMs are imported.<br><br>"
                       "Copyright © 2026 Thomas Cherryhomes — GPL-3.0-or-later<br>"
                       "<a href=\"https://fujinet.online/\">fujinet.online</a> · "
                       "<a href=\"https://github.com/FujiNetWIFI/fujinet-go-msdos-desktop/issues\">Report an issue</a>")
            .arg(QStringLiteral(MSDOS_VERSION_STRING)));
}

/* ---- view, status --------------------------------------------------------------------- */

void MainWindow::togglePref(const char *key, bool on)
{
    const int i = msdossession_pref_find(key);
    if (i >= 0)
        msdossession_pref_set_int(m_session, i, on);
    msdossession_settings_flush(m_session);
    applyView();
}

void MainWindow::applyView()
{
    const bool smooth = msdossession_smooth(m_session) != 0;
    const bool integer = msdossession_integer_scale(m_session) != 0;
    m_display->setSmooth(smooth);
    m_display->setIntegerScale(integer);
    if (m_aspectAction) m_aspectAction->setChecked(msdossession_get_int(m_session, "aspect43", 1) != 0);
    if (m_integerAction) m_integerAction->setChecked(integer);
    if (m_smoothAction) m_smoothAction->setChecked(smooth);
}

/* After Preferences or an import: the menus, the status and the page follow
 * what changed. */
void MainWindow::settingsChanged()
{
    applyView();
    msdosControllersRefresh();
    updateActions();
    updateStatus();
    showPage();
}

void MainWindow::updateStatus()
{
    bool on = false;
    QString text;
    if (!msdossession_is_running(m_session)) {
        text = msdossession_has_bios(m_session) ? QStringLiteral("Off") : QStringLiteral("No BIOS");
    } else {
        char buf[512];
        msdossession_status(m_session, buf, sizeof buf);
        text = local(buf);
        on = msdossession_fujinet_link(m_session) != 0;
    }
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
        m_status->update();
    }
    if (on != m_dotOn || m_dot->styleSheet().isEmpty()) {
        m_dotOn = on;
        m_dot->setStyleSheet(on ? QStringLiteral("background: %1; border-radius: 6px; "
                                                 "border: 1px solid rgba(128, 128, 128, 160);")
                                      .arg(msdosAccentColor().name())
                                : QStringLiteral("background: rgba(128, 128, 128, 64); border-radius: 6px;"));
    }
}

/* The drives' lights. */
void MainWindow::updateLights()
{
    const bool up = msdossession_is_running(m_session) != 0;
    for (int i = 0; i < 4; ++i) {
        bool present, on;
        if (i < 2) {
            msdos_floppy_info fi;
            if (msdossession_floppy_info(m_session, i, &fi) != 0) fi = msdos_floppy_info();
            present = fi.present;
            on = fi.active;
        } else {
            msdos_hd_info hi;
            if (msdossession_hd_info(m_session, i - 2, &hi) != 0) hi = msdos_hd_info();
            present = hi.path[0] != 0;
            on = hi.active;
        }
        m_lights[i]->setVisible(present && up);
        const QString style = on ? QStringLiteral("padding: 0 3px; border-radius: 3px; ") + msdosAccentStyle()
                                 : QStringLiteral("padding: 0 3px; border-radius: 3px;");
        if (m_lights[i]->styleSheet() != style)
            m_lights[i]->setStyleSheet(style);
    }
}

/* Gamepads come and go, FujiNet restarts itself, 86Box has something to
 * say: the other threads cannot touch the UI, so this timer asks. */
void MainWindow::fastTick()
{
    char text[1024];
    unsigned gen = msdossession_gamepad_generation(m_session);
    if (gen != m_padGeneration) {
        m_padGeneration = gen;
        if (msdossession_gamepad_last_event(m_session, text, sizeof text) > 0)
            toast(local(text));
    }
    gen = msdossession_fujinet_generation(m_session);
    if (gen != m_fujinetGeneration) {
        m_fujinetGeneration = gen;
        if (msdossession_fujinet_last_event(m_session, text, sizeof text) > 0)
            toast(local(text));
        updateStatus();
    }
    if (msdossession_take_notice(m_session, text, sizeof text) > 0)
        toast(local(text));
    updateLights();

    if (!m_shotPath.isEmpty() && m_display->frames() >= m_shotFrames) {
        if (!m_display->savePng(m_shotPath))
            qWarning("MSDOS_SCREENSHOT: could not write %s", qPrintable(m_shotPath));
        m_shotPath.clear();
    }
    if (m_quitFrames && m_display->frames() >= m_quitFrames) {
        m_quitFrames = 0;
        close();
    }
}

/* ---- events ------------------------------------------------------------------------------- */

bool MainWindow::handleKey(QKeyEvent *e, bool down)
{
    const Qt::KeyboardModifiers m = e->modifiers();
    const bool ctrl = m & Qt::ControlModifier, alt = m & Qt::AltModifier;
    if (down) {
        /* Ctrl+End lets the mouse go (86Box's own key for it) */
        if (ctrl && !alt && e->key() == Qt::Key_End && m_display->captured()) {
            m_display->setCaptured(false);
            return true;
        }
        if (ctrl && alt && e->key() == Qt::Key_PageDown) {
            if (!e->isAutoRepeat()) {
                if (isFullScreen()) showNormal(); else showFullScreen();
            }
            return true;
        }
    }
    return msdosForwardKey(m_session, e, down);
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    /* an open page other than the PC's has the keyboard */
    if (m_stack->currentWidget() != m_display || !handleKey(e, true))
        QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (!handleKey(e, false))
        QMainWindow::keyReleaseEvent(e);
}

/* Tab is the PC's while the picture shows. */
bool MainWindow::focusNextPrevChild(bool next)
{
    if (m_stack->currentWidget() == m_display)
        return false;
    return QMainWindow::focusNextPrevChild(next);
}

/* Focus gone: nothing stays held, and the mouse is the desktop's again. */
void MainWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && !isActiveWindow()) {
        if (!msdosControllersVisible())
            msdossession_release_keys(m_session);
        m_display->setCaptured(false);
    }
    QMainWindow::changeEvent(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty())
        return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty())
        loadMedia(path);
}

/* The panels are hidden rather than closed, so the main window going is
 * quitting. */
void MainWindow::closeEvent(QCloseEvent *e)
{
    m_statusTimer.stop();
    m_fastTimer.stop();
    QMainWindow::closeEvent(e);
    QCoreApplication::quit();
}
