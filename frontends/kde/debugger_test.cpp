/*
 * debugger_test -- the Debugger window (debugger/DebuggerWindow.cpp) driven
 * with synthetic events on the offscreen platform against a running PC (the
 * GLaBIOS fixture of core/tests, no IBM ROM) whose program is a hand-written
 * loop in RAM: the thirteen tabs and their order, the MSDOS_DEBUGGER_TAB names
 * and the older aliases, the Help contexts, the minimum size, attach/detach
 * with show and hide, the stepping keys and buttons, the console (transcript,
 * Tab completion, history), the disassembly (the CS:IP line, click /
 * double-click / F9 for a breakpoint, selection and Run to Cursor, the wheel,
 * the jump box), the register strip, Memory (Go to, Poke, search),
 * Breakpoints (all eight types, an expression condition), Watches, Trace and
 * History, Machine & I/O, Sound, Symbols, Interrupts, FujiNet and Video, pop-out
 * and dock, the env hooks and Ctrl+Shift+D from the main window.
 *
 * What needs a running PC skips without the BIOS fixture (or with
 * DBG_TEST_NO_BIOS=1, which makes the run behave as CI without it): the window
 * itself, its tabs, aliases, symbols and pop-out are checked either way.
 *
 * DBG_TEST_SHOTS=<dir> also grabs a PNG of each tab (DBG_TEST_LIGHT=1 /
 * DBG_TEST_DARK=1: in that scheme).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QtTest>

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QToolButton>

#include "MainWindow.h"
#include "debugger/Accent.h"
#include "debugger/DebuggerWindow.h"
#include "debugger/VideoTab.h"
#include "msdosdebug.h"
#include "msdosmemtools.h"
#include "msdossession.h"
#include "test_util.h"

namespace {

/* A loop in RAM at 2000:0000 (linear 20000), NOP-padded so a backward
 * disassembly guess is stable:
 *   0000  NOP NOP NOP
 *   0003  MOV AX,1234h
 *   0006  NOP
 *   0007  MOV [0500h],AX      ; a word write at 0000:0500
 *   000A  NOP
 *   000B  MOV DX,0080h
 *   000E  OUT DX,AL           ; port 80h
 *   000F  IN AL,DX
 *   0010  CALL 0020h
 *   0013  NOP NOP
 *   0015  JMP 0003h
 *   0018  NOP x8
 *   0020  INC CX ; RET */
const uint8_t kProg[] = {
    0x90, 0x90, 0x90, 0xB8, 0x34, 0x12, 0x90, 0xA3, 0x00, 0x05, 0x90, 0xBA, 0x80, 0x00, 0xEE, 0xEC,
    0xE8, 0x0D, 0x00, 0x90, 0x90, 0xE9, 0xEB, 0xFF, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
    0x41, 0xC3,
};
const uint16_t kSeg = 0x2000;
const uint32_t kBase = 0x20000;

QStringList expectedTabs()
{
    return { QStringLiteral("Console"), QStringLiteral("Disassembly"), QStringLiteral("Memory"),
             QStringLiteral("Breakpoints"), QStringLiteral("Watches"), QStringLiteral("Machine & I/O"),
             QStringLiteral("Trace"), QStringLiteral("History"), QStringLiteral("Video"),
             QStringLiteral("Sound"), QStringLiteral("Symbols"), QStringLiteral("Interrupts"),
             QStringLiteral("FujiNet") };
}

enum { T_CONSOLE, T_DISASM, T_MEMORY, T_BREAKS, T_WATCHES, T_MACHINE, T_TRACE, T_HISTORY, T_VIDEO, T_SOUND,
       T_SYMBOLS, T_INTS, T_FUJINET };

} // namespace

class DebuggerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    msdossession *m_s = nullptr;
    msdosdebug *m_d = nullptr;
    QMainWindow *m_win = nullptr;
    QTabWidget *m_tabs = nullptr;
    QString m_shots;
    bool m_machine = false;     /* a PC is running (the BIOS fixture was found) */

    static QMainWindow *findWindow()
    {
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->objectName() == QLatin1String("debugger")) return qobject_cast<QMainWindow *>(top);
        return nullptr;
    }
    /* the window is kept when closed: hide it (which detaches the engine) and
     * delete it, so msdosDebuggerShow() builds a fresh one that reads the env hooks */
    void discardWindow()
    {
        if (m_win) {
            m_win->hide();
            m_win->deleteLater();
        }
        m_win = nullptr;
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    void openWindow()
    {
        msdosDebuggerShow(nullptr, m_s);
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        if (m_machine) {
            QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_attached(m_d), 2000);
            waitStopped();
        }
    }
    QString tabText(int idx) const { return m_tabs->tabText(idx).replace(QStringLiteral("&&"), QStringLiteral("&")); }

    QPushButton *button(const QString &textStart)
    {
        for (QPushButton *btn : m_win->findChildren<QPushButton *>())
            if (btn->text().startsWith(textStart)) return btn;
        return nullptr;
    }
    QLineEdit *entry(const QString &placeholderStart)
    {
        for (QLineEdit *edit : m_win->findChildren<QLineEdit *>())
            if (edit->placeholderText().startsWith(placeholderStart)) return edit;
        return nullptr;
    }
    QPushButton *buttonIn(int tab, const QString &text)
    {
        for (QPushButton *btn : m_tabs->widget(tab)->findChildren<QPushButton *>())
            if (btn->text() == text) return btn;
        return nullptr;
    }
    QPlainTextEdit *disasmView()
    {
        for (QPlainTextEdit *view : m_win->findChildren<QPlainTextEdit *>())
            if (view->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return view;
        return nullptr;
    }
    QPlainTextEdit *consoleView() { return m_tabs->widget(T_CONSOLE)->findChild<QPlainTextEdit *>(); }
    QString consoleText()
    {
        auto *view = consoleView();
        return view ? view->toPlainText() : QString();
    }
    QLabel *statusLabel()
    {
        for (QLabel *label : m_win->findChildren<QLabel *>())
            if (label->text().startsWith(QLatin1String("Stopped")) || label->text() == QLatin1String("Running") ||
                label->text().startsWith(QLatin1String("The PC is not running")))
                return label;
        return nullptr;
    }
    QCheckBox *check(const QString &text)
    {
        for (QCheckBox *box : m_win->findChildren<QCheckBox *>())
            if (box->text() == text) return box;
        return nullptr;
    }
    QList<QLineEdit *> regFields()
    {
        QList<QLineEdit *> out;
        for (QLineEdit *edit : m_win->findChildren<QLineEdit *>())
            if (edit->toolTip().startsWith(QLatin1String("Enter sets it"))) out << edit;
        return out;
    }
    QTableWidget *bpTable() { return m_tabs->widget(T_BREAKS)->findChild<QTableWidget *>(); }
    msdos_cpu_regs regsNow()
    {
        msdos_cpu_regs regs;
        memset(&regs, 0, sizeof regs);
        msdosdebug_regs(m_d, &regs);
        return regs;
    }
    uint32_t pcOf() { return msdosdebug_linear(m_d, regsNow().cs, regsNow().eip); }

    void waitStopped()
    {
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_stopped(m_d), 4000);
        /* the window follows within its poll */
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel() && statusLabel()->text().startsWith(QLatin1String("Stopped")), 3000);
    }
    void shot(const QString &name)
    {
        if (!m_shots.isEmpty()) m_win->grab().save(m_shots + QLatin1Char('/') + name + QStringLiteral(".png"));
    }

    /* The block of the disassembly that shows an offset of the test segment (whatever
     * seg:off the listing spells that linear address with), or -1. */
    int lineOf(QPlainTextEdit *view, uint32_t off)
    {
        int line = 0;
        for (QTextBlock block = view->document()->begin(); block.isValid(); block = block.next(), ++line) {
            const QString text = block.text();
            bool segOk = false, offOk = false;
            const uint32_t seg = text.mid(3, 4).toUInt(&segOk, 16);
            const uint32_t offset = text.mid(8, 4).toUInt(&offOk, 16);
            if (segOk && offOk && text.mid(7, 1) == QLatin1String(":") && (seg << 4) + offset == kBase + off) return line;
        }
        return -1;
    }
    QPoint lineAt(QPlainTextEdit *view, int line, int column)
    {
        QTextCursor cur(view->document()->findBlockByNumber(line));
        cur.setPosition(cur.block().position() + column);
        const QRect rect = view->cursorRect(cur);
        return QPoint(rect.left() + 1, rect.center().y());
    }
    void clickLine(uint32_t off, int column, bool dbl = false)
    {
        QPlainTextEdit *view = disasmView();
        const int line = lineOf(view, off);
        QVERIFY2(line >= 0, qPrintable(QStringLiteral("line for %1").arg(off, 0, 16)));
        const QPoint pos = lineAt(view, line, column);
        if (dbl) QTest::mouseDClick(view->viewport(), Qt::LeftButton, {}, pos);
        else QTest::mouseClick(view->viewport(), Qt::LeftButton, {}, pos);
    }
    void typeInto(QLineEdit *edit, const QString &text)
    {
        edit->clear();
        QTest::keyClicks(edit, text);
        QTest::keyClick(edit, Qt::Key_Return);
    }
    /* The loop in RAM, with the registers it expects (interrupts off, the stack
     * in low RAM), stopped at 2000:offset. */
    bool setPc(uint32_t off)
    {
        msdos_cpu_regs regs = regsNow();
        char err[200] = "";
        regs.cs = kSeg;
        regs.eip = off;
        regs.ds = regs.es = regs.ss = 0;
        regs.esp = 0x7000;
        regs.eflags = 2;
        return msdosdebug_regs_set(m_d, &regs, err, sizeof err) && pcOf() == kBase + off;
    }
    bool loadProgram()
    {
        uint8_t back[sizeof kProg];
        if (msdosdebug_write(m_d, kBase, kProg, int(sizeof kProg)) < 0) return false;
        if (msdosdebug_read(m_d, kBase, back, int(sizeof kProg)) < 0 || memcmp(back, kProg, sizeof kProg)) return false;
        return setPc(3);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        qputenv("SDL_AUDIODRIVER", "dummy");
        if (qEnvironmentVariableIsSet("DBG_TEST_SHOTS")) m_shots = qEnvironmentVariable("DBG_TEST_SHOTS");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (qEnvironmentVariableIsSet("DBG_TEST_LIGHT"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
        if (qEnvironmentVariableIsSet("DBG_TEST_DARK"))
            QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
        const bool wantMachine = !qEnvironmentVariableIsSet("DBG_TEST_NO_BIOS") &&
                                 QFile::exists(QStringLiteral(MSDOS_TEST_FIXTURES "/roms/machines/glabios/GLABIOS_0.4.0_8X.ROM"));
        m_s = test_session_ex("kdedbg", 0, wantMachine ? 1 : 0);
        QVERIFY(m_s);
        m_d = msdossession_debugger(m_s);
        QVERIFY(m_d);
        if (wantMachine) {
            msdossession_start_opts opts = { 0, 0, 0 };
            m_machine = msdossession_start(m_s, &opts) == 0;
            if (!m_machine) qWarning("the PC did not start: %s", msdossession_last_error(m_s));
        }

        /* the window opens on the tab MSDOS_DEBUGGER_TAB names (the older "cpu" is the Disassembly) */
        qputenv("MSDOS_DEBUGGER_TAB", "cpu");
        msdosDebuggerShow(nullptr, m_s);
        qunsetenv("MSDOS_DEBUGGER_TAB");
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QVERIFY(m_tabs);
        QCOMPARE(m_tabs->currentIndex(), int(T_DISASM));
        if (m_machine) {
            waitStopped();
            QVERIFY(loadProgram());
        }
    }

    void cleanupTestCase()
    {
        delete m_win;       /* detaches */
        if (m_machine) msdossession_stop(m_s);
        msdossession_free(m_s);
    }

    void init()
    {
        if (!m_machine) return;
        if (!m_win->isVisible()) msdosDebuggerToggle(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_attached(m_d), 2000);
        if (!msdosdebug_stopped(m_d)) msdosdebug_stop(m_d);
        waitStopped();
        msdosdebug_bp_clear(m_d);
        if (QCheckBox *follow = check(QStringLiteral("Follow CS:IP"))) follow->setChecked(true);
        QVERIFY(loadProgram());
    }

    void tabsInOrder()
    {
        QStringList names;
        for (int idx = 0; idx < m_tabs->count(); ++idx) names << tabText(idx);
        QCOMPARE(names, expectedTabs());
        QVERIFY(m_win->findChild<VideoTab *>() != nullptr);
        /* the register strip is under every tab, not in one */
        const QList<QLineEdit *> regs = regFields();
        QCOMPARE(regs.size(), 9 + 6);
        for (QLineEdit *edit : regs) QVERIFY(!m_tabs->isAncestorOf(edit));
        QCOMPARE(m_win->findChildren<QCheckBox *>().size() >= 9, true);
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        QVERIFY(msdosdebug_attached(m_d));
        QVERIFY(msdosdebug_stopped(m_d));
        /* Run is the accent while stopped, whichever text it wears */
        QPushButton *run = button(QStringLiteral("Run (F5)"));
        QVERIFY(run);
        QVERIFY(run->styleSheet().contains(msdosDebugAccentFill(run).name()));
        QTest::mouseClick(run, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!msdosdebug_stopped(m_d), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(run->text() == QLatin1String("Stop (F5)"), 2000);
        QVERIFY(run->styleSheet().isEmpty());
        QCOMPARE(statusLabel()->text(), QStringLiteral("Running"));
        QTest::mouseClick(run, Qt::LeftButton);
        waitStopped();
        QVERIFY(run->styleSheet().contains(msdosDebugAccentFill(run).name()));
        QVERIFY(run->styleSheet().contains(msdosDebugAccentFillInk(run).name()));
    }

    void tabsByName()
    {
        /* each name the env hook takes, in a window of its own */
        discardWindow();
        struct { const char *name; int index; } cases[] = {
            { "cpu", T_DISASM }, { "registers", T_MEMORY }, { "io", T_MACHINE }, { "MACHINE", T_MACHINE },
            { "3", 3 }, { "Sound", T_SOUND }, { "console", T_CONSOLE }, { "hist", T_HISTORY },
            { "sym", T_SYMBOLS }, { "int", T_INTS }, { "fuji", T_FUJINET }, { "disassembly", T_DISASM },
            { "video", T_VIDEO }, { "watch", T_WATCHES }, { "trace", T_TRACE }, { "i/o", T_MACHINE } };
        for (const auto &one : cases) {
            qputenv("MSDOS_DEBUGGER_TAB", one.name);
            msdosDebuggerShow(nullptr, m_s);
            qunsetenv("MSDOS_DEBUGGER_TAB");
            QMainWindow *win = findWindow();
            QVERIFY(win);
            QVERIFY2(win->findChild<QTabWidget *>()->currentIndex() == one.index, one.name);
            m_win = win;
            discardWindow();
        }
        openWindow();
    }

    void helpContexts()
    {
        static const char *const kContexts[] = { "console", "disassembly", "memory", "breakpoints", "watches",
                                                 "machine", "trace", "history", "video", "sound", "symbols",
                                                 "interrupts", "fujinet" };
        for (int idx = 0; idx < 13; ++idx) {
            m_tabs->setCurrentIndex(idx);
            QCOMPARE(m_win->property("msdosHelpContext").toString(),
                     QStringLiteral("debugger/%1").arg(QLatin1String(kContexts[idx])));
        }
    }

    void fitsASmallScreen()
    {
        QVERIFY2(m_win->minimumSizeHint().height() <= 800,
                 qPrintable(QStringLiteral("minimum height %1").arg(m_win->minimumSizeHint().height())));
        QVERIFY2(m_win->minimumSizeHint().width() <= 1150,
                 qPrintable(QStringLiteral("minimum width %1").arg(m_win->minimumSizeHint().width())));
    }

    /* labels fill the list the first time the tab opens; Break at toggles; a row goes to the Disassembly */
    void symbolsTab()
    {
        msdosdebug_clear_symbols(m_d);
        QCOMPARE(msdosdebug_set_label(m_d, kBase + 3, "start_here"), 0);
        QCOMPARE(msdosdebug_set_label(m_d, kBase + 0x20, "bump_cx"), 0);
        m_tabs->setCurrentIndex(T_SYMBOLS);
        QListWidget *list = m_tabs->widget(T_SYMBOLS)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 2, 2000);
        QLineEdit *filter = m_tabs->widget(T_SYMBOLS)->findChild<QLineEdit *>();
        QVERIFY(filter);
        filter->setText(QStringLiteral("BUMP"));
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 1, 2000);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("bump_cx")));
        QVERIFY(list->item(0)->text().startsWith(QStringLiteral("20020")));
        /* Break at toggles an execute breakpoint on the selected symbol */
        list->setCurrentRow(0);
        QPushButton *breakAt = buttonIn(T_SYMBOLS, QStringLiteral("Break at"));
        QVERIFY(breakAt);
        breakAt->click();
        QVERIFY(msdosdebug_bp_at(m_d, kBase + 0x20) != 0);
        breakAt->click();
        QCOMPARE(msdosdebug_bp_at(m_d, kBase + 0x20), 0);
        /* activating a row shows it in the Disassembly */
        emit list->itemActivated(list->item(0));
        QCOMPARE(m_tabs->currentIndex(), int(T_DISASM));
        if (m_machine) {
            QPlainTextEdit *view = disasmView();
            QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0x20) >= 0, 2000);
            QVERIFY(view->toPlainText().contains(QStringLiteral("bump_cx")));
        }
        m_tabs->setCurrentIndex(T_SYMBOLS);
        filter->clear();
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 2, 2000);
        /* Clear Symbols */
        QPushButton *clear = buttonIn(T_SYMBOLS, QStringLiteral("Clear Symbols"));
        QVERIFY(clear);
        clear->click();
        QTRY_COMPARE_WITH_TIMEOUT(list->count(), 0, 2000);
        QCOMPARE(msdosdebug_symbols_count(m_d), 0);
        QVERIFY(entry(QStringLiteral("0000")) != nullptr);     /* the load segment */
        m_tabs->setCurrentIndex(T_CONSOLE);
    }

    void popOutAndDock()
    {
        m_tabs->setCurrentIndex(T_SOUND);
        QPushButton *pop = button(QStringLiteral("Pop Out"));
        QVERIFY(pop);
        pop->click();
        QCOMPARE(m_tabs->count(), 12);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        QCOMPARE(window->property("msdosHelpContext").toString(), QStringLiteral("debugger/sound"));
        for (QPushButton *btn : window->findChildren<QPushButton *>())
            if (btn->text() == QLatin1String("Dock")) btn->click();
        QCOMPARE(m_tabs->count(), 13);
        QCOMPARE(tabText(T_SOUND), QStringLiteral("Sound"));
    }

    void stepKeys()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        QCOMPARE(pcOf(), kBase + 3);
        QTest::keyClick(m_win, Qt::Key_F7);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(), kBase + 6, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().startsWith(QLatin1String("Stopped: step")), 2000);
        /* F8 steps over the CALL */
        QVERIFY(setPc(0x10));
        QTest::keyClick(m_win, Qt::Key_F8);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(), kBase + 0x13, 3000);
        /* the buttons do the same: Step Out of the routine */
        QVERIFY(setPc(0x20));
        {
            const uint8_t ret[2] = { 0x13, 0x00 };     /* a return address, as a CALL left it */
            msdosdebug_write(m_d, 0x6FFE, ret, 2);
            msdos_cpu_regs regs = regsNow();
            char err[100] = "";
            regs.esp = 0x6FFE;
            QVERIFY(msdosdebug_regs_set(m_d, &regs, err, sizeof err));
        }
        QTest::mouseClick(button(QStringLiteral("Step Out")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(), kBase + 0x13, 3000);
        /* stepping while running is refused with a word, not silently */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!msdosdebug_stopped(m_d), 2000);
        QTest::keyClick(m_win, Qt::Key_F7);
        QVERIFY(!msdosdebug_stopped(m_d) || statusLabel()->text() == QLatin1String("Running"));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
    }

    void consoleCommands()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        m_tabs->setCurrentIndex(T_CONSOLE);
        QLineEdit *input = entry(QStringLiteral("u cs:ip"));
        QPlainTextEdit *out = consoleView();
        QVERIFY(input && out);
        typeInto(input, QStringLiteral("r"));
        QVERIFY(out->toPlainText().contains(QStringLiteral("-r")));
        QVERIFY(out->toPlainText().contains(QStringLiteral("AX=")));
        QVERIFY(input->text().isEmpty());
        typeInto(input, QStringLiteral("print $10 + 2"));
        QTRY_VERIFY(consoleText().contains(QStringLiteral("$0012")));
        /* a command that moves the machine moves the window */
        typeInto(input, QStringLiteral("bp 2000:000A"));
        QVERIFY(msdosdebug_bp_at(m_d, kBase + 0x0A) != 0);
        typeInto(input, QStringLiteral("g"));
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_stopped(m_d) && pcOf() == kBase + 0x0A, 4000);
        waitStopped();
        QTRY_VERIFY_WITH_TIMEOUT(out->toPlainText().contains(QStringLiteral("— breakpoint")), 3000);
        QVERIFY(statusLabel()->text().contains(QStringLiteral("breakpoint")));
        QVERIFY(statusLabel()->text().contains(QStringLiteral("2000:000A")));
        /* Tab completes a command, lists several, and Up recalls */
        input->clear();
        QTest::keyClicks(input, QStringLiteral("pri"));
        QTest::keyClick(input, Qt::Key_Tab);
        QCOMPARE(input->text(), QStringLiteral("print "));
        input->clear();
        QTest::keyClicks(input, QStringLiteral("b"));
        QTest::keyClick(input, Qt::Key_Tab);
        QVERIFY(out->toPlainText().contains(QStringLiteral("bpw")));
        QVERIFY(input->text().startsWith(QLatin1Char('b')));
        input->clear();
        QTest::keyClick(input, Qt::Key_Up);
        QCOMPARE(input->text(), QStringLiteral("g"));
        QTest::keyClick(input, Qt::Key_Up);
        QCOMPARE(input->text(), QStringLiteral("bp 2000:000A"));
        QTest::keyClick(input, Qt::Key_Down);
        QCOMPARE(input->text(), QStringLiteral("g"));
        input->clear();
        shot(QStringLiteral("console"));
    }

    void disassemblyHighlights()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        m_tabs->setCurrentIndex(T_DISASM);
        QPlainTextEdit *view = disasmView();
        QVERIFY(view);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 3) >= 0, 2000);
        const int pcLine = lineOf(view, 3);
        QCOMPARE(view->document()->findBlockByNumber(pcLine).text().left(2), QStringLiteral(" >"));
        /* follow keeps it about a third of the way down */
        const int rows = view->document()->blockCount();
        QVERIFY(pcLine <= rows / 3 + 3);     /* near the top: nothing above 2000:0000 to show */
        /* the CS:IP line: the accent fill (the light variant on a dark theme), applied last */
        auto sels = view->extraSelections();
        QVERIFY(!sels.isEmpty());
        const auto &pcSel = sels.last();
        QCOMPARE(pcSel.cursor.blockNumber(), pcLine);
        QVERIFY(pcSel.format.boolProperty(QTextFormat::FullWidthSelection));
        QCOMPARE(pcSel.format.background().color(), msdosDebugAccentFill(view));
        QCOMPARE(pcSel.format.foreground().color(), msdosDebugAccentFillInk(view));

        /* a breakpoint's dot: filled and red, grey when disabled */
        msdosdebug_bp_toggle(m_d, kBase + 7);
        QTRY_VERIFY_WITH_TIMEOUT(view->document()->findBlockByNumber(lineOf(view, 7)).text().startsWith(
                                     QStringLiteral("●")), 2000);
        auto dotColor = [&](int line) {
            QColor color;
            for (const auto &sel : view->extraSelections())
                if (sel.cursor.blockNumber() == line && sel.cursor.hasSelection() &&
                    sel.cursor.selectionStart() == sel.cursor.block().position() &&
                    sel.cursor.selectionEnd() == sel.cursor.block().position() + 1)
                    color = sel.format.foreground().color();
            return color;
        };
        QCOMPARE(dotColor(lineOf(view, 7)), QColor(0xe0, 0x1b, 0x24));
        msdos_breakpoint bps[2];
        QCOMPARE(msdosdebug_bp_list(m_d, bps, 2), 1);
        msdosdebug_bp_enable(m_d, bps[0].id, 0);
        QTRY_COMPARE_WITH_TIMEOUT(dotColor(lineOf(view, 7)), QColor(0x9a, 0x99, 0x96), 2000);
        shot(QStringLiteral("disassembly"));
    }

    void clickSelectsAndGutterToggles()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        discardWindow();            /* a fresh window: nothing selected */
        openWindow();
        QVERIFY(loadProgram());
        m_tabs->setCurrentIndex(T_DISASM);
        QPlainTextEdit *view = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0x0B) >= 0, 2000);
        /* nothing selected: Run to Cursor and F9 say so */
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Select a line")));
        /* a click on the text selects (underlined, bold) and sets no breakpoint */
        clickLine(0x0B, 30);
        QCOMPARE(msdosdebug_bp_at(m_d, kBase + 0x0B), 0);
        {
            bool underlined = false;
            for (const auto &sel : view->extraSelections())
                if (sel.cursor.blockNumber() == lineOf(view, 0x0B) && sel.format.fontUnderline() &&
                    sel.format.fontWeight() == QFont::Bold)
                    underlined = true;
            QVERIFY(underlined);
        }
        clickLine(0x0E, 30);
        QCOMPARE(msdosdebug_bp_at(m_d, kBase + 0x0B), 0);
        QCOMPARE(msdosdebug_bp_at(m_d, kBase + 0x0E), 0);
        /* the left edge toggles, a double-click toggles, F9 toggles the selection */
        clickLine(0x0F, 0);
        QVERIFY(msdosdebug_bp_at(m_d, kBase + 0x0F) != 0);
        clickLine(0x0F, 0);
        QCOMPARE(msdosdebug_bp_at(m_d, kBase + 0x0F), 0);
        clickLine(0x10, 30, true);
        QVERIFY(msdosdebug_bp_at(m_d, kBase + 0x10) != 0);
        msdosdebug_bp_toggle(m_d, kBase + 0x10);
        clickLine(0x15, 30);
        QTest::keyClick(m_win, Qt::Key_F9);
        QVERIFY(msdosdebug_bp_at(m_d, kBase + 0x15) != 0);
        QTest::keyClick(m_win, Qt::Key_F9);
        QCOMPARE(msdosdebug_bp_at(m_d, kBase + 0x15), 0);
        msdos_breakpoint bps[4];
        QCOMPARE(msdosdebug_bp_list(m_d, bps, 4), 0);
    }

    void runToCursor()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        m_tabs->setCurrentIndex(T_DISASM);
        QPlainTextEdit *view = disasmView();
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0x0F) >= 0, 2000);
        clickLine(0x0F, 30);
        QTest::mouseClick(button(QStringLiteral("Run to Cursor")), Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(), kBase + 0x0F, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0x0F) >= 0, 2000);
    }

    void wheelJumpAndFollow()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        m_tabs->setCurrentIndex(T_DISASM);
        QPlainTextEdit *view = disasmView();
        QCheckBox *follow = check(QStringLiteral("Follow CS:IP"));
        QVERIFY(follow && follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 3) >= 0, 2000);
        const QPoint centre = view->viewport()->rect().center();
        QWheelEvent wheel(centre, view->viewport()->mapToGlobal(centre), QPoint(), QPoint(0, -120), Qt::NoButton, {},
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &wheel);
        QVERIFY(!follow->isChecked());
        /* the jump box: seg:off, a symbol, then nonsense */
        QLineEdit *jump = entry(QStringLiteral("F000:FFF0"));
        QVERIFY(jump);
        typeInto(jump, QStringLiteral("2000:0020"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0x20) >= 0, 2000);
        msdosdebug_set_label(m_d, kBase + 0x10, "back");
        typeInto(jump, QStringLiteral("back"));
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 0x10) >= 0, 2000);
        QVERIFY(view->toPlainText().contains(QStringLiteral("back")));
        typeInto(jump, QStringLiteral("nosuchlabel"));
        QVERIFY(!m_win->statusBar()->currentMessage().isEmpty());
        /* Follow CS:IP again returns to the PC */
        QTest::mouseClick(follow, Qt::LeftButton, {}, QPoint(4, follow->height() / 2));
        QVERIFY(follow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(lineOf(view, 3) >= 0, 2000);
        msdosdebug_set_label(m_d, kBase + 0x10, "");
    }

    void registerStrip()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        /* under every tab: the Disassembly and the Breakpoints show it as well */
        for (int tab : { T_CONSOLE, T_DISASM, T_BREAKS, T_FUJINET }) {
            m_tabs->setCurrentIndex(tab);
            QVERIFY(regFields().first()->isVisible());
        }
        m_tabs->setCurrentIndex(T_MEMORY);
        QList<QLineEdit *> regs = regFields();
        QCOMPARE(regs.size(), 15);          /* AX..IP, then CS DS ES SS FS GS */
        QTRY_COMPARE_WITH_TIMEOUT(regs[8]->text().toUpper(), QStringLiteral("0003"), 2000);   /* IP */
        QCOMPARE(regs[9]->text().toUpper(), QStringLiteral("2000"));                           /* CS */
        typeInto(regs[8], QStringLiteral("0010"));
        QTRY_COMPARE_WITH_TIMEOUT(pcOf(), kBase + 0x10, 2000);
        typeInto(regs[0], QStringLiteral("beef"));
        QCOMPARE(regsNow().eax & 0xFFFFu, 0xBEEFu);
        /* an expression, the register taken from another */
        typeInto(regs[1], QStringLiteral("ax + 1"));
        QCOMPARE(regsNow().ebx & 0xFFFFu, 0xBEF0u);
        typeInto(regs[2], QStringLiteral("$1234"));
        QCOMPARE(regsNow().ecx & 0xFFFFu, 0x1234u);
        typeInto(regs[10], QStringLiteral("0040"));       /* DS */
        QCOMPARE(int(regsNow().ds), 0x40);
        /* the flags */
        QCheckBox *carry = check(QStringLiteral("CF"));
        QVERIFY(carry);
        msdosdebug_set_reg(m_d, "cf", 0);
        QTRY_VERIFY_WITH_TIMEOUT(!carry->isChecked(), 2000);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        QCOMPARE(int(regsNow().eflags & 1), 1);
        QTest::mouseClick(carry, Qt::LeftButton, {}, QPoint(6, carry->height() / 2));
        QCOMPARE(int(regsNow().eflags & 1), 0);
        QCheckBox *zero = check(QStringLiteral("ZF"));
        QVERIFY(zero);
        QTest::mouseClick(zero, Qt::LeftButton, {}, QPoint(6, zero->height() / 2));
        QCOMPARE(int((regsNow().eflags >> 6) & 1), 1);
        QTest::mouseClick(zero, Qt::LeftButton, {}, QPoint(6, zero->height() / 2));
        /* the segment bases show */
        bool bases = false;
        for (QLabel *label : m_win->findChildren<QLabel *>())
            bases |= label->text() == QLatin1String("base 20000");
        QVERIFY(bases);
        /* edits need a stopped machine */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(!msdosdebug_stopped(m_d), 2000);
        typeInto(regs[8], QStringLiteral("0003"));
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("Stop the PC")));
        QTest::keyClick(m_win, Qt::Key_F5);
        waitStopped();
        shot(QStringLiteral("registers"));
    }

    void memoryPokeAndSearch()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        m_tabs->setCurrentIndex(T_MEMORY);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QLineEdit *addr = entry(QStringLiteral("0040:0017"));
        QLineEdit *val = entry(QStringLiteral("value"));
        QVERIFY(addr && val);
        addr->setText(QStringLiteral("2000:0040"));
        typeInto(val, QStringLiteral("5a"));
        uint8_t byte = 0;
        msdosdebug_read(m_d, kBase + 0x40, &byte, 1);
        QCOMPARE(int(byte), 0x5A);
        addr->setText(QStringLiteral("20041"));       /* a linear address, hex */
        typeInto(val, QStringLiteral("$A5"));
        msdosdebug_read(m_d, kBase + 0x41, &byte, 1);
        QCOMPARE(int(byte), 0xA5);
        const uint8_t eleven = 0x11;
        msdosdebug_write(m_d, kBase + 0x42, &eleven, 1);
        addr->setText(QStringLiteral("2000:0042"));
        typeInto(val, QStringLiteral("1FF"));        /* not a byte: refused */
        msdosdebug_read(m_d, kBase + 0x42, &byte, 1);
        QCOMPARE(int(byte), 0x11);
        QVERIFY(m_win->statusBar()->currentMessage().contains(QLatin1String("byte")));
        /* Go to marks the byte in the accent, in the row of its page */
        QLineEdit *goEntry = entry(QStringLiteral("ds:si"));
        QVERIFY(goEntry);
        typeInto(goEntry, QStringLiteral("2000:0003"));
        auto *mem = m_tabs->widget(T_MEMORY)->findChildren<QPlainTextEdit *>().last();
        QVERIFY(mem->toPlainText().contains(QStringLiteral("20000: 90 90 90 B8 34 12")));
        bool marked = false;
        for (const auto &sel : mem->extraSelections())
            marked |= sel.format.background().color() == msdosDebugAccentFill(mem) &&
                      sel.format.foreground().color() == msdosDebugAccentFillInk(mem);
        QVERIFY(marked);
        /* the rows are labelled with the region they are in */
        QVERIFY2(mem->toPlainText().contains(QStringLiteral("; ")), "region names");
        /* search finds the program's MOV [0500],AX */
        typeInto(goEntry, QStringLiteral("2000:0000"));
        QLineEdit *find = entry(QStringLiteral("bytes or text"));
        QVERIFY(find);
        typeInto(find, QStringLiteral("A3 00 05 90 BA"));
        QLabel *note = nullptr;
        for (QLabel *label : m_tabs->widget(T_MEMORY)->findChildren<QLabel *>())
            if (label->text().startsWith(QLatin1String("Found")) || label->text().startsWith(QLatin1String("Not found"))) note = label;
        QVERIFY(note);
        QVERIFY2(note->text().contains(QStringLiteral("Found at 20007")), qPrintable(note->text()));
        /* a miss */
        auto *kind = m_tabs->widget(T_MEMORY)->findChild<QComboBox *>();
        QVERIFY(kind);
        kind->setCurrentIndex(1);
        typeInto(find, QStringLiteral("zzzzqqxx"));
        QVERIFY2(note->text().startsWith(QLatin1String("Not found")), qPrintable(note->text()));
        kind->setCurrentIndex(0);
        shot(QStringLiteral("memory"));
    }

    void breakpointsTab()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        m_tabs->setCurrentIndex(T_BREAKS);
        QTableWidget *table = bpTable();
        QVERIFY(table);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        QCOMPARE(table->rowCount(), 0);
        QLineEdit *start = entry(QStringLiteral("1234:0010"));
        QLineEdit *end = entry(QStringLiteral("(end)"));
        QLineEdit *ahEntry = entry(QStringLiteral("AH"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(start && end && ahEntry && cond);
        QComboBox *type = m_tabs->currentWidget()->findChild<QComboBox *>();
        QVERIFY(type);
        QCOMPARE(type->count(), 8);
        QCOMPARE(type->itemText(0), QStringLiteral("Execute"));
        QCOMPARE(type->itemText(7), QStringLiteral("INT"));
        QPushButton *add = buttonIn(T_BREAKS, QStringLiteral("Add"));
        QPushButton *clear = buttonIn(T_BREAKS, QStringLiteral("Clear All"));
        QLabel *msg = nullptr;
        for (QLabel *label : m_tabs->currentWidget()->findChildren<QLabel *>())
            if (label->styleSheet().contains(QLatin1String("e01b24"))) msg = label;
        QVERIFY(add && clear && msg);
        QVERIFY(!ahEntry->isEnabled());

        /* exec at a seg:off address */
        typeInto(start, QStringLiteral("2000:000F"));
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QVERIFY(table->item(0, 2)->text().startsWith(QStringLiteral("Execute")));
        QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
        QVERIFY(start->text().isEmpty());
        /* a memory write breakpoint with a condition on the byte written */
        type->setCurrentIndex(2);
        start->setText(QStringLiteral("0000:0500"));
        cond->setText(QStringLiteral("value > 0"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 2, 2000);
        QVERIFY(table->item(1, 2)->text().contains(QStringLiteral("rite")));
        QCOMPARE(table->item(1, 3)->text(), QStringLiteral("value > 0"));
        /* errors are shown, not swallowed */
        cond->setText(QStringLiteral("nosuchname == 1"));
        start->setText(QStringLiteral("0000:0600"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(table->rowCount(), 2);
        cond->clear();
        msg->clear();
        /* ports and INT: the type enables AH, a bad port is refused */
        type->setCurrentIndex(4);       /* Port IN */
        QVERIFY(!ahEntry->isEnabled());
        start->setText(QStringLiteral("12345"));
        QTest::mouseClick(add, Qt::LeftButton);
        QVERIFY(!msg->text().isEmpty());
        QCOMPARE(table->rowCount(), 2);
        start->setText(QStringLiteral("80"));
        QTest::mouseClick(add, Qt::LeftButton);
        type->setCurrentIndex(5);       /* Port OUT */
        start->setText(QStringLiteral("80"));
        end->setText(QStringLiteral("8F"));
        QTest::mouseClick(add, Qt::LeftButton);
        type->setCurrentIndex(6);       /* Port IN/OUT */
        start->setText(QStringLiteral("2F8"));
        end->clear();
        QTest::mouseClick(add, Qt::LeftButton);
        type->setCurrentIndex(3);       /* Memory read/write */
        start->setText(QStringLiteral("0000:0700"));
        QTest::mouseClick(add, Qt::LeftButton);
        type->setCurrentIndex(1);       /* Memory read */
        start->setText(QStringLiteral("0000:0710"));
        QTest::mouseClick(add, Qt::LeftButton);
        type->setCurrentIndex(7);       /* INT */
        QVERIFY(ahEntry->isEnabled());
        QVERIFY(!end->isEnabled());
        start->setText(QStringLiteral("21"));
        ahEntry->setText(QStringLiteral("09"));
        QTest::mouseClick(add, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 8, 2000);
        msdos_breakpoint list[12];
        QCOMPARE(msdosdebug_bp_list(m_d, list, 12), 8);
        QCOMPARE(list[2].type, int(MSDOS_BP_IN));
        QCOMPARE(list[3].type, int(MSDOS_BP_OUT));
        QCOMPARE(int(list[3].end), 0x8F);
        QCOMPARE(list[4].type, int(MSDOS_BP_IO));
        QCOMPARE(list[5].type, int(MSDOS_BP_ACCESS));
        QCOMPARE(list[6].type, int(MSDOS_BP_READ));
        QCOMPARE(list[7].type, int(MSDOS_BP_INT));
        QCOMPARE(list[7].ah, 9);
        QCOMPARE(int(list[7].start), 0x21);
        /* an edited condition goes to the engine */
        table->item(1, 3)->setText(QStringLiteral("value > 1"));
        QTRY_VERIFY_WITH_TIMEOUT((msdosdebug_bp_list(m_d, list, 12),
                                  QString::fromUtf8(list[1].condition) == QLatin1String("value > 1")), 2000);
        for (int idx = 7; idx >= 2; --idx) msdosdebug_bp_remove(m_d, list[idx].id);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 2, 2000);
        start->clear();
        end->clear();
        ahEntry->clear();
        msg->clear();
        type->setCurrentIndex(0);

        /* the write breakpoint stops the run first, after the store at 2000:0007, in words */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_stopped(m_d) && pcOf() == kBase + 0x0A, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("wrote 00500 = $34 (was $")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("wrote 00500")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(table->item(1, 4) && table->item(1, 4)->text().toInt() >= 1, 2000);
        /* then the exec breakpoint, with its hits counted */
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_stopped(m_d) && pcOf() == kBase + 0x0F, 4000);
        QTRY_VERIFY_WITH_TIMEOUT(statusLabel()->text().contains(QLatin1String("2000:000F")), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(table->item(0, 4) && table->item(0, 4)->text().toInt() >= 1, 2000);
        /* Clear Hits zeroes the counts */
        QPushButton *hits = button(QStringLiteral("Clear Hits"));
        QVERIFY(hits);
        QTest::mouseClick(hits, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 4)->text(), QStringLiteral("0"), 2000);
        /* unticking it lets the run pass; the write breakpoint stops it again */
        table->item(0, 0)->setCheckState(Qt::Unchecked);
        QTRY_COMPARE_WITH_TIMEOUT(msdosdebug_bp_list(m_d, list, 12), 2, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(msdosdebug_bp_list(m_d, list, 12) == 2 ? list[0].enabled : 1, 0, 2000);
        QCOMPARE(list[1].enabled, 1);
        QVERIFY(setPc(3));
        QTest::keyClick(m_win, Qt::Key_F5);
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_stopped(m_d) && pcOf() == kBase + 0x0A, 5000);
        shot(QStringLiteral("breakpoints"));
        /* the row's delete button */
        auto *remove = qobject_cast<QToolButton *>(table->cellWidget(1, 5));
        QVERIFY(remove);
        QTest::mouseClick(remove, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 2000);
        QCOMPARE(msdosdebug_bp_list(m_d, list, 12), 1);
        QTest::mouseClick(clear, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 0, 2000);
        QCOMPARE(msdosdebug_bp_list(m_d, list, 12), 0);
    }

    /* a write breakpoint with a condition on the word, stopping the program in the core's words */
    void breakpointWithCondition()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        m_tabs->setCurrentIndex(T_BREAKS);
        QLineEdit *addr = entry(QStringLiteral("1234:0010"));
        QLineEdit *cond = entry(QStringLiteral("condition"));
        QVERIFY(addr && cond);
        addr->setText(QStringLiteral("0000:0500"));
        cond->setText(QStringLiteral("value == $34 && old != $34"));
        QComboBox *type = m_tabs->widget(T_BREAKS)->findChild<QComboBox *>();
        type->setCurrentIndex(2);   /* Memory write */
        buttonIn(T_BREAKS, QStringLiteral("Add"))->click();
        msdos_breakpoint bps[4];
        QCOMPARE(msdosdebug_bp_list(m_d, bps, 4), 1);
        QCOMPARE(bps[0].type, int(MSDOS_BP_WRITE));
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value == $34 && old != $34"));
        /* a condition that does not parse is refused */
        addr->setText(QStringLiteral("0000:0501"));
        cond->setText(QStringLiteral("value =="));
        buttonIn(T_BREAKS, QStringLiteral("Add"))->click();
        QCOMPARE(msdosdebug_bp_list(m_d, bps, 4), 1);
        type->setCurrentIndex(0);
        /* the program's first store changes the low byte from 0 (a word is two byte accesses): it
         * stops after the store, in the core's words */
        const uint8_t zero[2] = { 0, 0 };
        msdosdebug_write(m_d, 0x500, zero, 2);
        msdosdebug_resume(m_d);
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_stopped(m_d), 4000);
        msdos_stop_snapshot snap;
        msdosdebug_stop_snapshot(m_d, &snap);
        QVERIFY2(QString::fromUtf8(snap.text).contains(QStringLiteral("= $34 (was $00)")), snap.text);
        QTRY_VERIFY_WITH_TIMEOUT(button(QStringLiteral("Run (F5)")) != nullptr, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(consoleText().contains(QStringLiteral("= $34 (was $00)")), 2000);
        msdosdebug_bp_clear(m_d);
    }

    void watchesAndBreakOnChange()
    {
        msdoswatch_clear();
        m_tabs->setCurrentIndex(T_WATCHES);
        QLineEdit *entryBox = m_tabs->widget(T_WATCHES)->findChild<QLineEdit *>();
        QVERIFY(entryBox);
        entryBox->setText(QStringLiteral("0000:0500"));
        QTest::keyClick(entryBox, Qt::Key_Return);
        QCOMPARE(msdoswatch_count(), 1);
        QListWidget *list = m_tabs->widget(T_WATCHES)->findChild<QListWidget *>();
        QVERIFY(list);
        QTRY_COMPARE(list->count(), 1);
        list->setCurrentRow(0);
        if (m_machine) {
            QVERIFY(list->item(0)->text().contains(QStringLiteral("00500")));
            buttonIn(T_WATCHES, QStringLiteral("Break on Change"))->click();
            msdos_breakpoint bps[4];
            QTRY_COMPARE(msdosdebug_bp_list(m_d, bps, 4), 1);
            QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
            QCOMPARE(bps[0].type, int(MSDOS_BP_WRITE));
            /* Memory shows the watch, marked */
            buttonIn(T_WATCHES, QStringLiteral("Memory"))->click();
            QCOMPARE(m_tabs->currentIndex(), int(T_MEMORY));
            m_tabs->setCurrentIndex(T_WATCHES);
        }
        /* Remove */
        buttonIn(T_WATCHES, QStringLiteral("Remove"))->click();
        QCOMPARE(msdoswatch_count(), 0);
        QTRY_COMPARE(list->count(), 0);
        if (m_machine) msdosdebug_bp_clear(m_d);
        msdoswatch_clear();
    }

    void traceAndHistory()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        m_tabs->setCurrentIndex(T_HISTORY);
        QPushButton *record = buttonIn(T_HISTORY, QStringLiteral("Record"));
        QVERIFY(record);
        record->click();
        QVERIFY(msdosdebug_history_enabled(m_d));
        QVERIFY(loadProgram());
        for (int idx = 0; idx < 4; ++idx) {
            msdosdebug_step(m_d);
            QTest::qWait(80);
        }
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_history_count(m_d) >= 4, 3000);
        m_tabs->setCurrentIndex(T_CONSOLE);
        m_tabs->setCurrentIndex(T_HISTORY);
        auto *text = m_tabs->widget(T_HISTORY)->findChild<QPlainTextEdit *>();
        QVERIFY(text);
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("2000:0003")), 3000);
        QVERIFY(text->toPlainText().contains(QStringLiteral("nop")));
        /* the filter: branches only leaves the NOPs out */
        auto *filter = m_tabs->widget(T_HISTORY)->findChild<QComboBox *>();
        QVERIFY(filter);
        filter->setCurrentIndex(1);
        QTRY_VERIFY_WITH_TIMEOUT(!text->toPlainText().contains(QStringLiteral("nop")), 3000);
        filter->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(text->toPlainText().contains(QStringLiteral("nop")), 3000);
        record->click();
        QVERIFY(!msdosdebug_history_enabled(m_d));
        msdosdebug_history_clear(m_d);
        /* Trace is live: Record on, run, instructions arrive */
        m_tabs->setCurrentIndex(T_TRACE);
        QPushButton *traceRec = buttonIn(T_TRACE, QStringLiteral("Record"));
        QVERIFY(traceRec);
        traceRec->click();
        QVERIFY(msdosdebug_trace_enabled(m_d));
        QVERIFY(setPc(3));
        msdosdebug_resume(m_d);
        auto *live = m_tabs->widget(T_TRACE)->findChild<QPlainTextEdit *>();
        QTRY_VERIFY_WITH_TIMEOUT(live->toPlainText().contains(QStringLiteral("inc cx")), 6000);
        shot(QStringLiteral("trace"));
        traceRec->click();
        QVERIFY(!msdosdebug_trace_enabled(m_d));
        msdosdebug_trace_clear(m_d);
        msdosdebug_stop(m_d);
        waitStopped();
    }

    void machineSoundAndTheRest()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        m_tabs->setCurrentIndex(T_VIDEO);
        VideoTab *video = m_win->findChild<VideoTab *>();
        QVERIFY(video);
        video->refresh(true);
        QVERIFY(video->findChild<QComboBox *>() != nullptr);
        shot(QStringLiteral("video"));
        m_tabs->setCurrentIndex(T_SOUND);
        auto soundText = [this] {
            auto *view = qobject_cast<QPlainTextEdit *>(m_tabs->widget(T_SOUND));
            return view ? view->toPlainText() : QString();
        };
        QTRY_VERIFY2_WITH_TIMEOUT(soundText().contains(QStringLiteral("PC speaker")), qPrintable(soundText()), 3000);
        shot(QStringLiteral("sound"));
        m_tabs->setCurrentIndex(T_MACHINE);
        QString machineText;
        QTRY_VERIFY_WITH_TIMEOUT(!m_tabs->widget(T_MACHINE)->findChildren<QPlainTextEdit *>().isEmpty(), 2000);
        for (QPlainTextEdit *view : m_tabs->widget(T_MACHINE)->findChildren<QPlainTextEdit *>()) machineText += view->toPlainText();
        QVERIFY2(machineText.contains(QStringLiteral("Memory map")), qPrintable(machineText));
        QVERIFY2(machineText.contains(QStringLiteral("Conventional")), qPrintable(machineText));
        shot(QStringLiteral("machine"));
        /* Interrupts: the vector table is filled; Trace on records an INT */
        m_tabs->setCurrentIndex(T_INTS);
        auto vectorsShown = [this] {
            for (QPlainTextEdit *view : m_tabs->widget(T_INTS)->findChildren<QPlainTextEdit *>())
                if (view->toPlainText().startsWith(QStringLiteral("00 "))) return true;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(vectorsShown(), 2000);
        QCheckBox *intTrace = m_tabs->widget(T_INTS)->findChild<QCheckBox *>();
        QVERIFY(intTrace);
        shot(QStringLiteral("interrupts"));
        /* FujiNet: empty without the runtime, and says so */
        m_tabs->setCurrentIndex(T_FUJINET);
        QTRY_VERIFY_WITH_TIMEOUT(m_tabs->currentWidget()->isVisible(), 1000);
        bool note = false;
        for (QLabel *label : m_tabs->currentWidget()->findChildren<QLabel *>())
            note |= label->text().contains(QLatin1String("FujiNet is off"));
        QVERIFY(note);
        shot(QStringLiteral("fujinet"));
        m_tabs->setCurrentIndex(T_CONSOLE);
    }

    /* MSDOS_DEBUGGER_POPOUT peels a tab off at the start */
    void popoutEnv()
    {
        discardWindow();
        qputenv("MSDOS_DEBUGGER_POPOUT", "sound");
        msdosDebuggerShow(nullptr, m_s);
        qunsetenv("MSDOS_DEBUGGER_POPOUT");
        m_win = findWindow();
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
        QCOMPARE(m_tabs->count(), 12);
        QWidget *window = nullptr;
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) window = top;
        QVERIFY(window);
        discardWindow();
        for (QWidget *top : QApplication::topLevelWidgets())
            if (top->windowTitle().endsWith(QLatin1String("Sound"))) top->close();
        openWindow();
    }

    /* the video env hooks reach the tab */
    void videoEnv()
    {
        discardWindow();
        qputenv("MSDOS_DEBUGGER_TAB", "video");
        qputenv("MSDOS_DEBUGGER_VIDEO_VIEW", "beam");
        qputenv("MSDOS_DEBUGGER_VIDEO_ZOOM", "2");
        msdosDebuggerShow(nullptr, m_s);
        qunsetenv("MSDOS_DEBUGGER_TAB");
        qunsetenv("MSDOS_DEBUGGER_VIDEO_VIEW");
        qunsetenv("MSDOS_DEBUGGER_VIDEO_ZOOM");
        m_win = findWindow();
        QVERIFY(m_win);
        QVERIFY(QTest::qWaitForWindowExposed(m_win));
        m_tabs = m_win->findChild<QTabWidget *>();
        QCOMPARE(m_tabs->currentIndex(), int(T_VIDEO));
        QVERIFY(m_win->findChild<VideoTab *>());
        if (m_machine) waitStopped();
        QTest::qWait(300);
        shot(QStringLiteral("video-env"));
        discardWindow();
        openWindow();
    }

    /* MSDOS_DEBUGGER_WATCH, _WATCH_BREAK, _MEM_GOTO and _HISTORY act in a fresh window */
    void envHooks()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        msdosdebug_bp_clear(m_d);
        msdoswatch_clear();
        discardWindow();
        qputenv("MSDOS_DEBUGGER_TAB", "watches");
        qputenv("MSDOS_DEBUGGER_WATCH", "0000:0500");
        qputenv("MSDOS_DEBUGGER_WATCH_BREAK", "0000:0500");
        qputenv("MSDOS_DEBUGGER_MEM_GOTO", "0000:0500");
        qputenv("MSDOS_DEBUGGER_HISTORY", "1");
        msdosDebuggerShow(nullptr, m_s);
        qunsetenv("MSDOS_DEBUGGER_TAB");
        qunsetenv("MSDOS_DEBUGGER_WATCH");
        qunsetenv("MSDOS_DEBUGGER_WATCH_BREAK");
        qunsetenv("MSDOS_DEBUGGER_MEM_GOTO");
        qunsetenv("MSDOS_DEBUGGER_HISTORY");
        m_win = findWindow();
        QVERIFY(m_win);
        m_tabs = m_win->findChild<QTabWidget *>();
        QTRY_COMPARE_WITH_TIMEOUT(msdoswatch_count(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_history_enabled(m_d), 3000);
        msdos_breakpoint bps[4];
        QTRY_COMPARE_WITH_TIMEOUT(msdosdebug_bp_list(m_d, bps, 4), 1, 3000);
        QCOMPARE(QString::fromUtf8(bps[0].condition), QStringLiteral("value != old"));
        /* MEM_GOTO put its address in the Memory view (the tab stays the one asked for) */
        QCOMPARE(m_tabs->currentIndex(), int(T_WATCHES));
        m_tabs->setCurrentIndex(T_MEMORY);
        auto *dump = m_tabs->widget(T_MEMORY)->findChildren<QPlainTextEdit *>().last();
        QTRY_VERIFY_WITH_TIMEOUT(dump->toPlainText().contains(QStringLiteral("00500:")), 3000);
        msdosdebug_history_enable(m_d, 0);
        msdosdebug_bp_clear(m_d);
        msdoswatch_clear();
    }

    /* DBG_TEST_SHOTS=<dir>: a PNG of every tab */
    void shots()
    {
        if (m_shots.isEmpty()) QSKIP("DBG_TEST_SHOTS not set");
        for (int idx = 0; idx < m_tabs->count(); ++idx) {
            m_tabs->setCurrentIndex(idx);
            QTest::qWait(250);
            QVERIFY(m_win->grab().save(QStringLiteral("%1/tab%2.png").arg(m_shots).arg(idx)));
        }
    }

    /* Ctrl+Shift+D from the main window, through the View menu's action. */
    void viewMenuFromTheMainWindow()
    {
        /* the display is a QRhiWidget; with no GPU device (CI's offscreen Qt)
         * the window swaps in the software display */
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.vert.qsb")));
        QVERIFY(QFile::exists(QStringLiteral(":/shaders/display.frag.qsb")));
        MainWindow main(m_s);
        main.show();
        QVERIFY(QTest::qWaitForWindowExposed(&main));
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::qWait(300);   /* let a queued renderFailed swap the display */
        if (!m_win->isVisible()) msdosDebuggerShow(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QAction *debugger = nullptr;
        for (QAction *action : main.findChildren<QAction *>())
            if (action->text().remove(QLatin1Char('&')) == QLatin1String("Debugger")) debugger = action;
        QVERIFY(debugger);
        QCOMPARE(debugger->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D));
        debugger->trigger();
        QVERIFY(!m_win->isVisible());
        if (m_machine) QVERIFY(!msdosdebug_attached(m_d));
        debugger->trigger();
        QVERIFY(m_win->isVisible());
        if (m_machine) {
            QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_attached(m_d), 2000);
            waitStopped();
        }
    }

    /* Closing the window detaches (the PC runs on); showing it attaches (stopping) again. */
    void hideDetaches()
    {
        if (!m_machine) QSKIP("needs a running PC (the GLaBIOS fixture)");
        QVERIFY(m_win->isVisible());
        m_win->close();
        QVERIFY(!m_win->isVisible());
        QVERIFY(!msdosdebug_attached(m_d));
        QTRY_VERIFY_WITH_TIMEOUT(!msdosdebug_stopped(m_d), 3000);
        msdosDebuggerToggle(nullptr, m_s);
        QVERIFY(m_win->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_attached(m_d), 2000);
        waitStopped();
        msdosDebuggerToggle(nullptr, m_s);
        QVERIFY(!m_win->isVisible());
        QVERIFY(!msdosdebug_attached(m_d));
        msdosDebuggerToggle(nullptr, m_s);
        QTRY_VERIFY_WITH_TIMEOUT(msdosdebug_attached(m_d), 2000);
        waitStopped();
    }
};

QTEST_MAIN(DebuggerTest)
#include "debugger_test.moc"
