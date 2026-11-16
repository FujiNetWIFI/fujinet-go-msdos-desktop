/*
 * Debugger window (Qt6 Widgets) over the session's debugger engine, via
 * core/include/msdosdebug.h. The family's tabbed design (the Adam, Coleco, MSX
 * and INTV KDE debuggers): a QTabWidget under a toolbar, the editable register
 * strip under every tab. Mirrors the GNOME one tab for tab.
 *
 * Tabs: the Console (DEBUG.COM's commands and the family's, with a history and
 * Tab completion), the Disassembly (click a line to select it for Run to
 * Cursor; click its left edge, double-click it or press F9 for its breakpoint),
 * Memory (any linear address, Go to, Poke, search), Breakpoints (execute, memory,
 * I/O ports and software interrupts, each with an expression condition),
 * Watches, Machine & I/O (the CPU, memory map, interrupt controller, timer,
 * DMA, ports, disks and cards), Trace (live) and History (what ran before the
 * stop), Video (the card's picture decoded from its memory, registers,
 * palette, memory layout and beam; see msdosvidtab.h), Sound (the PC speaker
 * and the FM chip), Symbols (linker maps), Interrupts (a DOS/BIOS call trace and
 * the vector table) and FujiNet (the FujiBus packets on its COM port and its
 * UART). The toolbar carries the stepping controls, Save and Pop Out; the
 * family's keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step
 * out, F9 breakpoint).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); hiding it detaches and lets the machine run on.
 *
 * A short timer polls the engine's stop snapshot and refreshes the tabs
 * showing when it moved; while the machine runs, the live tabs refresh
 * twice a second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "Accent.h"
#include "VideoTab.h"
#include "../help/HelpWindow.h"
#include "msdosdebug.h"
#include "msdosmemtools.h"

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define MEM_PAGE (MEM_ROWS * 16)
#define BP_MAX 128
#define TRACE_MAX 512
#define CONSOLE_LINES 5000
#define HIST_FETCH 4000         /* ring entries scanned for a Trace / History view */
#define HIST_SHOWN 400          /* matching lines shown */
#define REGION_MAX 16

namespace {

enum { TAB_CONSOLE, TAB_DISASM, TAB_MEMORY, TAB_BREAKS, TAB_WATCHES, TAB_MACHINE, TAB_TRACE, TAB_HISTORY,
       TAB_VIDEO, TAB_SOUND, TAB_SYMBOLS, TAB_INTS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names MSDOS_DEBUGGER_TAB and MSDOS_DEBUGGER_POPOUT take, and
 * their labels (which they take too); the Help context of each is its name. */
const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "disassembly", "Disassembly" }, { "memory", "Memory" },
    { "breakpoints", "Breakpoints" }, { "watches", "Watches" }, { "machine", "Machine & I/O" },
    { "trace", "Trace" }, { "history", "History" }, { "video", "Video" }, { "sound", "Sound" },
    { "symbols", "Symbols" }, { "interrupts", "Interrupts" }, { "fujinet", "FujiNet" },
};

/* The older names of the tabs that were folded in. */
const struct { const char *name; int tab; } kTabAliases[] = {
    { "cpu", TAB_DISASM }, { "registers", TAB_MEMORY }, { "io", TAB_MACHINE }, { "ints", TAB_INTS },
};

/* the general registers, as msdosdebug_register_set names them */
const char *const kRegs16[] = { "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "ip" };
const char *const kRegs32[] = { "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip" };
constexpr int NREGS = 9;
const char *const kSregs[] = { "cs", "ds", "es", "ss", "fs", "gs" };
constexpr int NSREGS = 6;
/* the flags shown, with their EFLAGS bits */
const struct { const char *name; int bit; } kFlags[] = {
    { "of", 11 }, { "df", 10 }, { "if", 9 }, { "tf", 8 }, { "sf", 7 }, { "zf", 6 },
    { "af", 4 }, { "pf", 2 }, { "cf", 0 },
};
constexpr int NFLAGS = 9;

const QString kGray = QStringLiteral("color: gray;");

/* The accent: the Run button while stopped, the CS:IP line, a marked row. On a
 * dark theme the light variant with black ink (Accent.h). */
QTextCharFormat accentFormat(const QWidget *wid)
{
    QTextCharFormat fmt;
    fmt.setBackground(msdosDebugAccentFill(wid));
    fmt.setForeground(msdosDebugAccentFillInk(wid));
    return fmt;
}

void setMono(QWidget *widget)
{
    QFont font = widget->font();
    font.setFamily(QStringLiteral("monospace"));
    font.setStyleHint(QFont::TypeWriter);
    widget->setFont(font);
}

QPlainTextEdit *monoView()
{
    auto *view = new QPlainTextEdit;
    view->setReadOnly(true);
    setMono(view);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    return view;
}

/* Engine text for a view: control bytes dropped. */
QString cleanText(const QString &raw)
{
    QString out;
    out.reserve(raw.size());
    for (const QChar letter : raw)
        if (letter.unicode() >= 0x20 || letter == QLatin1Char('\n') || letter == QLatin1Char('\t')) out += letter;
    return out;
}
QString cleanText(const char *chars) { return cleanText(QString::fromUtf8(chars)); }

QString hexText(quint64 value, int digits)
{
    return QStringLiteral("%1").arg(value, digits, 16, QLatin1Char('0')).toUpper();
}

Q_ATTRIBUTE_FORMAT_PRINTF(2, 3)
void appendf(QString &text, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    text += QString::vasprintf(fmt, args);
    va_end(args);
}

QLabel *titleLabel(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QLabel *hintLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setStyleSheet(kGray);
    label->setWordWrap(true);
    return label;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *entry = new QLineEdit;
    entry->setPlaceholderText(placeholder);
    entry->setFixedWidth(entry->fontMetrics().averageCharWidth() * chars + 16);
    return entry;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *entry, int digits)
{
    entry->setFixedWidth(entry->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *view)
{
    return view->verticalScrollBar()->value() >= view->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *view, const QString &text, bool follow = false)
{
    if (view->toPlainText() == text) {
        if (follow) view->verticalScrollBar()->setValue(view->verticalScrollBar()->maximum());
        return;
    }
    const int top = view->verticalScrollBar()->value(), left = view->horizontalScrollBar()->value();
    view->setPlainText(text);
    view->verticalScrollBar()->setValue(follow ? view->verticalScrollBar()->maximum() : top);
    view->horizontalScrollBar()->setValue(left);
}

/* A whole line of a view in a format: the accent fill, a mark. */
QTextEdit::ExtraSelection lineSelection(QPlainTextEdit *view, int line, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection sel;
    sel.cursor = QTextCursor(view->document()->findBlockByNumber(line));
    sel.format = fmt;
    sel.format.setProperty(QTextFormat::FullWidthSelection, true);
    return sel;
}

/* A line that elides instead of widening the window (the CPU line). */
class ElidedLabel : public QLabel {
public:
    ElidedLabel() { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QRect area = contentsRect();
        painter.setPen(palette().color(foregroundRole()));
        painter.drawText(area, (alignment() & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, area.width()));
    }
};

/* The status line never widens the window. It is clipped, not elided: the stop
 * reason ("wrote 0046C = $41 (was $40) at F000:E0A1") is read whole in the
 * tooltip and the Breakpoints tab, and never turns into "...". */
class StatusLabel : public QLabel {
public:
    StatusLabel() { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
};

/* MSDOS_DEBUGGER_TAB and MSDOS_DEBUGGER_POPOUT: a page number, or a tab's name or label
 * (any case), or the start of one, or of a word of a label ("machine"), or an
 * older name (cpu, registers, io ...). */
int tabFromEnv(const char *variable = "MSDOS_DEBUGGER_TAB")
{
    const QString tab = qEnvironmentVariable(variable).trimmed();
    if (tab.isEmpty()) return -1;
    bool isNum = false;
    const int number = tab.toInt(&isNum);
    if (isNum) return number >= 0 && number < TAB_COUNT ? number : -1;
    for (const auto &alias : kTabAliases)
        if (tab.compare(QString::fromUtf8(alias.name), Qt::CaseInsensitive) == 0) return alias.tab;
    for (int pass = 0; pass < 3; ++pass)
        for (int tabIdx = 0; tabIdx < TAB_COUNT; ++tabIdx) {
            const QString name = QString::fromUtf8(kTabs[tabIdx].name), label = QString::fromUtf8(kTabs[tabIdx].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return tabIdx;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return tabIdx;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int wordPos = 1; wordPos < words.size(); ++wordPos)
                    if (words[wordPos].startsWith(tab, Qt::CaseInsensitive)) return tabIdx;
            }
        }
    return -1;
}

/* The label a tab shows: "&" starts a mnemonic in tab text, so it is doubled. */
QString tabLabel(int tab)
{
    return QString::fromUtf8(kTabs[tab].label).replace(QLatin1Char('&'), QLatin1String("&&"));
}

/* The family's keys: F5 run/stop, F7 step, F8 step over, Shift+F8 step out, F9
 * breakpoint. (F12 is the PC's.) */
bool isDebuggerKey(const QKeyEvent *evt)
{
    const Qt::KeyboardModifiers mods = evt->modifiers() & ~Qt::KeypadModifier;
    switch (evt->key()) {
    case Qt::Key_F5: case Qt::Key_F7: case Qt::Key_F9: return mods == Qt::NoModifier;
    case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

/* A tab's window: closing it docks the tab again. The family's keys work in
 * it too, handed to the debugger. */
class PopWindow : public QWidget {
public:
    std::function<void()> onClose;
    std::function<bool(QKeyEvent *)> onKey;
protected:
    void closeEvent(QCloseEvent *evt) override
    {
        if (onClose) onClose();
        QWidget::closeEvent(evt);
    }
    bool event(QEvent *evt) override
    {
        if (evt->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(evt))) {
            evt->accept();
            return true;
        }
        return QWidget::event(evt);
    }
    void keyPressEvent(QKeyEvent *evt) override
    {
        if (!onKey || !onKey(evt)) QWidget::keyPressEvent(evt);
    }
};

class DebuggerWindow : public QMainWindow {
public:
    DebuggerWindow(msdossession *session, QWidget *parent);
    ~DebuggerWindow() override;
    /* Attaches (stopping the machine) and refreshes. */
    void attachAndRefresh();
    void firstShow();

protected:
    bool event(QEvent *evt) override;
    void keyPressEvent(QKeyEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    bool eventFilter(QObject *obj, QEvent *evt) override;

private:
    QWidget *buildToolbar();
    QWidget *buildRegs();
    QWidget *buildConsole();
    QWidget *buildDisasm();
    QWidget *buildMemory();
    QWidget *buildBreaks();
    QWidget *buildWatches();
    QWidget *buildMachine();
    QWidget *buildTrace(int which);
    QWidget *buildSound();
    QWidget *buildSymbols();
    QWidget *buildInts();
    QWidget *buildFujinet();

    void refreshStatus(const msdos_stop_snapshot &snap);
    void refreshRegs();
    void refreshMem();
    void refreshRegions();
    void refreshDisasm();
    void refreshBps();
    void refreshWatches();
    void refreshMachine();
    void refreshTrace(int which, bool force);
    void refreshSound();
    void refreshSymbols(bool force);
    void refreshInts(bool force);
    void refreshFujinet(bool force);
    void refreshTab(int tab, bool live);
    void refreshAll();
    void resync();
    void syncLater();
    void settleAndSync();
    void tick();

    void consoleAppend(const QString &text);
    void consoleOutput(const QString &text);
    void consoleStop(const msdos_stop_snapshot &snap);
    void runConsole();
    void completeConsole();
    void showHistory();
    void say(const QString &msg);
    void message(const QString &msg);

    void toggleRun();
    void stepAnd(int (*func)(msdosdebug *));
    void runToCursor();
    void toggleBreakpoint(uint32_t linear);
    void toggleSelected();
    void addBreakpoint();
    void bpTypeChanged(int type);
    void addWatch();
    void setReg(int idx, bool segment);
    void setFlag(int idx, bool enabled);
    void memGoto();
    void memPoke();
    void memSearch(bool backward);
    void showInMemory(uint32_t first, uint32_t last);
    void showInDisasm(uint32_t linear);
    void jumpTo();
    void breakAtSymbol();
    void loadSymbols();
    void save(const char *kind, const QString &title, const QString &file);
    bool handleKey(QKeyEvent *evt);
    void popOut(int tab);
    void dock(int tab);
    int currentTab() const;
    bool tabVisible(int tab) const;
    void showTab(int tab);
    int visibleRows() const;
    uint32_t rowsBack(uint16_t seg, uint32_t off, int rows);
    uint32_t rowsOn(uint16_t seg, uint32_t off, int rows);
    bool addressOf(const QString &text, uint32_t *linear);
    bool numberOf(const QString &text, int64_t *out, QString *why);
    bool parseAddr(const QString &text, uint16_t *seg, uint32_t *off, uint32_t *lin);
    void applyRegister(const char *name, int64_t value, const QString &okMessage);
    void setEntry(QLineEdit *edit, const QString &text);
    void layoutRegs(bool wide);

    msdossession *m_session;
    msdosdebug *m_dbg;
    uint64_t m_seenSerial = ~0ull;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    bool m_attachedRunning = false;     /* attached while the machine was running */
    bool m_shownOnce = false;
    bool m_closing = false;
    int m_runningTicks = 0;
    QByteArray m_envWatch, m_envWatchBreak, m_envMemGoto;   /* the capture variables, read at construction */
    bool m_envHistory = false;
    QTimer m_timer;

    QTabWidget *m_tabs = nullptr;
    VideoTab *m_video = nullptr;
    StatusLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    /* the register strip */
    QGridLayout *m_regGrid = nullptr;
    QWidget *m_regCell[NREGS] = {};
    QLabel *m_regLabel[NREGS] = {};
    QLineEdit *m_reg[NREGS] = {};
    QWidget *m_sregCell[NSREGS] = {};
    QLineEdit *m_sreg[NSREGS] = {};
    QLabel *m_sbase[NSREGS] = {};
    QCheckBox *m_flag[NFLAGS] = {};
    ElidedLabel *m_cpuInfo = nullptr;
    bool m_is386 = false;

    /* console */
    QPlainTextEdit *m_conOut = nullptr;
    QLineEdit *m_conIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    /* memory */
    QPlainTextEdit *m_mem = nullptr;
    QLineEdit *m_memGoto = nullptr, *m_memAddr = nullptr, *m_memVal = nullptr;
    QComboBox *m_srchKind = nullptr;
    QLineEdit *m_srchEntry = nullptr;
    QLabel *m_srchNote = nullptr;
    uint32_t m_memTop = 0;              /* linear */
    int m_memPrefix = 7;                /* characters before the first byte of a row */
    std::vector<msdos_region> m_regions;      /* the memory map, fetched once per stop */
    uint64_t m_regionSerial = ~0ull;
    bool m_memHl = false;               /* a range is highlighted */
    uint32_t m_memHlLo = 0, m_memHlHi = 0;

    /* disassembly */
    QPlainTextEdit *m_disasm = nullptr;
    QCheckBox *m_followPc = nullptr;
    QLineEdit *m_jump = nullptr;
    uint16_t m_topSeg = 0xF000;         /* the first line */
    uint32_t m_topOff = 0xFFF0;
    int64_t m_selected = -1;            /* the selected line's linear address, or -1 */
    int m_wheel = 0;                    /* wheel travel not yet a row */
    std::vector<uint32_t> m_lineLin;

    /* breakpoints */
    QTableWidget *m_bpTable = nullptr;
    QString m_bpSig;                    /* what the table shows, to rebuild on change */
    bool m_bpFilling = false;
    QLabel *m_bpEmpty = nullptr, *m_bpMsg = nullptr, *m_bpStop = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpAh = nullptr, *m_bpCond = nullptr;

    /* watches */
    QLineEdit *m_wtEntry = nullptr;
    QListWidget *m_wtList = nullptr;

    /* Machine & I/O */
    QPlainTextEdit *m_mapText = nullptr, *m_ioText = nullptr;

    /* Trace (0, live) and History (1, while stopped): one ring */
    QPushButton *m_hRec[2] = {};
    QComboBox *m_hFilter[2] = {};
    QLineEdit *m_hRange[2] = {};
    QPlainTextEdit *m_hText[2] = {};
    QLabel *m_hNote[2] = {};
    uint64_t m_hSeen[2] = { ~0ull, ~0ull };
    bool m_hSync = false;

    /* sound, symbols */
    QPlainTextEdit *m_soundText = nullptr;
    QLineEdit *m_symFilter = nullptr, *m_symSeg = nullptr;
    QListWidget *m_symList = nullptr;
    QLabel *m_symCount = nullptr;
    unsigned m_symGen = ~0u;
    QString m_symShownFilter;

    /* interrupts */
    QCheckBox *m_intOn = nullptr;
    QPlainTextEdit *m_intText = nullptr, *m_ivtText = nullptr;
    uint64_t m_intLast = 0;
    int m_intCount = -1;

    /* fujinet */
    QPlainTextEdit *m_uartText = nullptr, *m_fujiText = nullptr;
    QLabel *m_fujiNote = nullptr;
    uint64_t m_fujiLast = 0;
    int m_fujiCount = -1;

    /* tabs peeled off into windows of their own */
    QWidget *m_pages[TAB_COUNT] = {};
    QWidget *m_popped[TAB_COUNT] = {};
};

QPointer<DebuggerWindow> s_win;

DebuggerWindow::DebuggerWindow(msdossession *session, QWidget *parent)
    : QMainWindow(parent, Qt::Window), m_session(session), m_dbg(msdossession_debugger(session))
{
    setObjectName(QStringLiteral("debugger"));
    setWindowTitle(QStringLiteral("Debugger"));
    resize(1150, 800);

    auto *central = new QWidget;
    auto *root = new QVBoxLayout(central);
    root->addWidget(buildToolbar());
    m_tabs = new QTabWidget;
    m_tabs->setUsesScrollButtons(true);
    QWidget *pages[TAB_COUNT];
    pages[TAB_CONSOLE] = buildConsole();
    pages[TAB_DISASM] = buildDisasm();
    pages[TAB_MEMORY] = buildMemory();
    pages[TAB_BREAKS] = buildBreaks();
    pages[TAB_WATCHES] = buildWatches();
    pages[TAB_MACHINE] = buildMachine();
    pages[TAB_TRACE] = buildTrace(0);
    pages[TAB_HISTORY] = buildTrace(1);
    pages[TAB_VIDEO] = m_video = new VideoTab(m_session);
    pages[TAB_SOUND] = buildSound();
    pages[TAB_SYMBOLS] = buildSymbols();
    pages[TAB_INTS] = buildInts();
    pages[TAB_FUJINET] = buildFujinet();
    for (int idx = 0; idx < TAB_COUNT; ++idx) {
        pages[idx]->setProperty("tab", idx);     /* each page keeps its identity when tabs move */
        m_pages[idx] = pages[idx];
        m_tabs->addTab(pages[idx], tabLabel(idx));
    }
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* Help's contexts (data/help/help.md), one per tab: Ctrl+Shift+H, F1 and the Help button */
    auto setHelpContext = [this]() {
        const int page = currentTab();
        if (page >= 0 && page < TAB_COUNT)
            setProperty("msdosHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[page].name)));
    };
    setHelpContext();
    new QShortcut(QKeySequence(Qt::Key_F1), this, [this] { msdosHelpFor(this); });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, setHelpContext](int) {
        setHelpContext();
        /* the view has no size until it shows: fill it now */
        refreshTab(currentTab(), false);
        if (currentTab() == TAB_CONSOLE) m_conIn->setFocus();
    });
    root->addWidget(m_tabs, 1);
    root->addWidget(buildRegs());
    setCentralWidget(central);
    statusBar();

    m_envWatch = qgetenv("MSDOS_DEBUGGER_WATCH");
    m_envWatchBreak = qgetenv("MSDOS_DEBUGGER_WATCH_BREAK");
    m_envMemGoto = qgetenv("MSDOS_DEBUGGER_MEM_GOTO");
    m_envHistory = qEnvironmentVariableIsSet("MSDOS_DEBUGGER_HISTORY");
    const int popTab = tabFromEnv("MSDOS_DEBUGGER_POPOUT");
    if (popTab >= 0) popOut(popTab);     /* for captures: peel a tab off at the start */

    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_timer.start(100);
}

/* Gone with the main window (the session outlives it): the machine runs on. */
DebuggerWindow::~DebuggerWindow()
{
    m_closing = true;
    m_timer.stop();
    msdosdebug_detach(m_dbg);
    for (QWidget *&win : m_popped)
        if (win) {
            QWidget *popped = win;
            win = nullptr;
            popped->close();
        }
}

/* ---- building ------------------------------------------------------------- */

QWidget *DebuggerWindow::buildToolbar()
{
    /* a tool bar, so a narrow window folds the buttons that do not fit into its extension menu */
    auto *bar = new QToolBar;
    bar->setMovable(false);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> action) {
        auto *button = new QPushButton(label);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(tip);
        connect(button, &QPushButton::clicked, this, action);
        bar->addWidget(button);
        return button;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"), [this] { stepAnd(msdosdebug_step); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a CALL, INT, LOOP or REP, to the instruction after it"),
        [this] { stepAnd(msdosdebug_step_over); });
    add(QStringLiteral("Step Out (Shift+F8)"), QStringLiteral("On until the routine returns (RET, RETF or IRET)"),
        [this] { stepAnd(msdosdebug_step_out); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);

    auto *saveBtn = new QPushButton(QStringLiteral("Save"));
    saveBtn->setFocusPolicy(Qt::NoFocus);
    auto *menu = new QMenu(saveBtn);
    menu->addAction(QStringLiteral("Disassembly (from CS:IP)"), this,
                    [this] { save("dis", QStringLiteral("Disassembly"), QStringLiteral("disassembly.asm")); });
    menu->addAction(QStringLiteral("Memory (the first megabyte)"), this,
                    [this] { save("mem", QStringLiteral("Memory"), QStringLiteral("memory.bin")); });
    menu->addAction(QStringLiteral("CPU registers"), this,
                    [this] { save("regs", QStringLiteral("CPU registers"), QStringLiteral("registers.txt")); });
    saveBtn->setMenu(menu);
    bar->addWidget(saveBtn);
    add(QStringLiteral("Pop Out"), QStringLiteral("Move the tab that shows into a window of its own; Dock puts it back"),
        [this] { popOut(currentTab()); });
    bar->addWidget(msdosHelpButton(QString()));

    /* the status line sits in the status bar: the toolbar has no room for it */
    m_status = new StatusLabel;
    m_status->setAlignment(Qt::AlignLeft);
    statusBar()->addWidget(m_status, 1);
    return bar;
}

/* The x86 registers, under every tab. Enter sets one (only while stopped); a flag
 * is a check box. The segment registers show their bases; the 32-bit names and
 * FS/GS only on a 386. */
QWidget *DebuggerWindow::buildRegs()
{
    auto *box = new QGroupBox(QStringLiteral("Registers"));
    auto *regs = new QGridLayout(box);
    m_regGrid = regs;
    regs->setHorizontalSpacing(8);
    regs->setVerticalSpacing(2);
    auto regEntry = [](int chars) {
        auto *entry = new QLineEdit;
        setMono(entry);
        entry->setMaxLength(24);
        setDigits(entry, chars);
        entry->setToolTip(QStringLiteral("Enter sets it (a bare number is hex; an expression such as ax + 1 works; "
                                         "only while stopped)"));
        return entry;
    };
    for (int idx = 0; idx < NREGS; ++idx) {
        auto *cell = new QWidget;
        auto *row = new QHBoxLayout(cell);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        m_regLabel[idx] = new QLabel(QString::fromLatin1(kRegs16[idx]).toUpper());
        m_regLabel[idx]->setStyleSheet(kGray);
        m_regLabel[idx]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_regLabel[idx]->setFixedWidth(m_regLabel[idx]->fontMetrics().horizontalAdvance(QStringLiteral("EAX")));
        m_reg[idx] = regEntry(4);
        connect(m_reg[idx], &QLineEdit::returnPressed, this, [this, idx] { setReg(idx, false); });
        row->addWidget(m_regLabel[idx]);
        row->addWidget(m_reg[idx]);
        m_regCell[idx] = cell;
        regs->addWidget(cell, 0, idx);
    }
    for (int idx = 0; idx < NSREGS; ++idx) {
        auto *cell = new QWidget;
        auto *row = new QHBoxLayout(cell);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        auto *name = new QLabel(QString::fromLatin1(kSregs[idx]).toUpper());
        name->setStyleSheet(kGray);
        m_sreg[idx] = regEntry(4);
        connect(m_sreg[idx], &QLineEdit::returnPressed, this, [this, idx] { setReg(idx, true); });
        row->addWidget(name);
        row->addWidget(m_sreg[idx]);
        row->addStretch(1);
        m_sregCell[idx] = cell;
        regs->addWidget(cell, 2, idx);
        m_sbase[idx] = new QLabel;
        m_sbase[idx]->setStyleSheet(kGray);
        QFont small = m_sbase[idx]->font();
        small.setPointSizeF((small.pointSizeF() > 0 ? small.pointSizeF() : 10) * 0.85);
        m_sbase[idx]->setFont(small);
        regs->addWidget(m_sbase[idx], 3, idx);
    }
    regs->setColumnStretch(NREGS, 1);

    auto *flags = new QHBoxLayout;
    for (int idx = 0; idx < NFLAGS; ++idx) {
        m_flag[idx] = new QCheckBox(QString::fromLatin1(kFlags[idx].name).toUpper());
        m_flag[idx]->setToolTip(QStringLiteral("Flag %1 of FLAGS (only while stopped)")
                                    .arg(QString::fromLatin1(kFlags[idx].name).toUpper()));
        connect(m_flag[idx], &QCheckBox::clicked, this, [this, idx](bool enabled) { setFlag(idx, enabled); });
        flags->addWidget(m_flag[idx]);
    }
    flags->addSpacing(16);
    m_cpuInfo = new ElidedLabel;
    m_cpuInfo->setStyleSheet(kGray);
    flags->addWidget(m_cpuInfo, 1);
    regs->addLayout(flags, 4, 0, 1, NREGS + 1);
    return box;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("DEBUG.COM's commands: r d e u g t p, plus bp/bpr/bpw/bpa/bpi/bpo/bpint, bl, "
                                          "bc, i, o, print, find, watch, trace, symbols, map, io, sound. Type h for "
                                          "the list; Tab completes, Up/Down recall.\n"));
    auto *row = new QHBoxLayout;
    auto *prompt = new QLabel(QStringLiteral("-"));
    setMono(prompt);
    m_conIn = new QLineEdit;
    setMono(m_conIn);
    m_conIn->setPlaceholderText(QStringLiteral("u cs:ip   d ds:0   bpo 2f8   bpint 21 9   g"));
    m_conIn->installEventFilter(this);
    connect(m_conIn, &QLineEdit::returnPressed, this, [this] { runConsole(); });
    row->addWidget(prompt);
    row->addWidget(m_conIn, 1);
    layout->addWidget(m_conOut, 1);
    layout->addLayout(row);
    return page;
}

/* The dump: Go to, a page back or on, Poke, and the search bar. */
QWidget *DebuggerWindow::buildMemory()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("ds:si / B800:0 / symbol"), 22);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    for (int dir = -1; dir <= 1; dir += 2) {
        auto *arrow = new QToolButton;
        arrow->setArrowType(dir < 0 ? Qt::UpArrow : Qt::DownArrow);
        arrow->setToolTip(dir < 0 ? QStringLiteral("Back 4K") : QStringLiteral("On 4K"));
        connect(arrow, &QToolButton::clicked, this, [this, dir] {
            if (dir < 0) m_memTop = m_memTop > MEM_PAGE ? m_memTop - MEM_PAGE : 0;
            else m_memTop = uint32_t(qMin<quint64>(quint64(m_memTop) + MEM_PAGE, 0xFFFFFFFFull - MEM_PAGE + 1));
            refreshMem();
        });
        nav->addWidget(arrow);
    }
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Poke")));
    m_memAddr = smallEntry(QStringLiteral("0040:0017 / symbol"), 18);
    m_memVal = smallEntry(QStringLiteral("value"), 10);
    m_memVal->setToolTip(QStringLiteral("A byte: hex, or an expression. Only while stopped; ROM ignores it."));
    auto *pokeBtn = new QPushButton(QStringLiteral("Poke"));
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memPoke(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memPoke(); });
    connect(pokeBtn, &QPushButton::clicked, this, [this] { memPoke(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(m_memVal);
    nav->addWidget(pokeBtn);
    nav->addStretch(1);
    layout->addLayout(nav);

    auto *search = new QHBoxLayout;
    search->addWidget(new QLabel(QStringLiteral("Search")));
    m_srchKind = new QComboBox;
    m_srchKind->addItems({ QStringLiteral("Hex bytes"), QStringLiteral("Text") });
    m_srchKind->setToolTip(QStringLiteral("Hex bytes (B8 34 12) or text, one byte per character"));
    search->addWidget(m_srchKind);
    m_srchEntry = new QLineEdit;
    m_srchEntry->setPlaceholderText(QStringLiteral("bytes or text"));
    connect(m_srchEntry, &QLineEdit::returnPressed, this, [this] { memSearch(false); });
    search->addWidget(m_srchEntry, 1);
    auto *next = new QPushButton(QStringLiteral("Find Next"));
    auto *prev = new QPushButton(QStringLiteral("Find Previous"));
    connect(next, &QPushButton::clicked, this, [this] { memSearch(false); });
    connect(prev, &QPushButton::clicked, this, [this] { memSearch(true); });
    search->addWidget(next);
    search->addWidget(prev);
    m_srchNote = new QLabel;
    m_srchNote->setStyleSheet(kGray);
    search->addWidget(m_srchNote);
    layout->addLayout(search);

    layout->addWidget(titleLabel(QStringLiteral("Memory (linear; video RAM at A0000-BFFFF reads as the CPU would)")));
    m_mem = monoView();
    layout->addWidget(m_mem, 1);
    return page;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *row = new QHBoxLayout;
    m_followPc = new QCheckBox(QStringLiteral("Follow CS:IP"));
    m_followPc->setChecked(true);
    connect(m_followPc, &QCheckBox::clicked, this, [this] { refreshDisasm(); });
    m_jump = smallEntry(QStringLiteral("F000:FFF0 or _main"), 20);
    connect(m_jump, &QLineEdit::returnPressed, this, [this] { jumpTo(); });
    row->addWidget(m_followPc);
    row->addSpacing(8);
    row->addWidget(new QLabel(QStringLiteral("Jump to")));
    row->addWidget(m_jump);
    row->addSpacing(8);
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge, "
                                            "double-click it or press F9 for a breakpoint; scroll to browse")),
                   1);
    layout->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    layout->addWidget(m_disasm, 1);
    return page;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    auto *top = new QHBoxLayout;
    top->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; memory, ports and INT after "
                                            "the instruction that touched them. Breakpoints are kept while the "
                                            "debugger is closed, but only act while it is open.")),
                   1);
    layout->addLayout(top);

    m_bpTable = new QTableWidget(0, 6);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("#"), QStringLiteral("Breakpoint"),
                                           QStringLiteral("Condition"), QStringLiteral("Hits"), QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::DoubleClicked);     /* only the Condition cells are editable */
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling) return;
        if (item->column() == 3) {
            /* an edited condition: the engine checks it, and an empty one removes it */
            char err[200] = "";
            const QByteArray text = item->text().trimmed().toUtf8();
            if (msdosdebug_bp_set_condition(m_dbg, item->data(Qt::UserRole).toInt(), text.constData(), err, sizeof err))
                m_bpMsg->clear();
            else
                m_bpMsg->setText(err[0] ? cleanText(err) : QStringLiteral("The condition could not be set"));
            m_bpSig.clear();
            syncLater();
            return;
        }
        if (item->column() != 0) return;
        msdosdebug_bp_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    layout->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    layout->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Execute"), QStringLiteral("Memory read"), QStringLiteral("Memory write"),
                         QStringLiteral("Memory read/write"), QStringLiteral("Port IN"), QStringLiteral("Port OUT"),
                         QStringLiteral("Port IN/OUT"), QStringLiteral("INT") });
    m_bpType->setToolTip(QStringLiteral("Memory read/write is a read or a write of the range; the port types take "
                                        "a port (0000-FFFF); INT an interrupt (00-FF) and an optional AH"));
    m_bpStart = smallEntry(QStringLiteral("1234:0010 / symbol"), 24);
    m_bpEnd = smallEntry(QStringLiteral("(end)"), 14);
    m_bpAh = smallEntry(QStringLiteral("AH (any)"), 10);
    m_bpAh->setEnabled(false);
    m_bpCond = new QLineEdit;
    m_bpCond->setPlaceholderText(QStringLiteral("condition (optional): value != old, ax == 5, [ds:si] > 3"));
    m_bpCond->setToolTip(QStringLiteral(
        "An expression: numbers ($B800), the registers (ax eax cs ...), flags (zf cf ...), symbols, [addr] "
        "(a byte) and {addr} (a word), seg:off, frame, cycles, and for a memory or port access value, addr and old "
        "(the byte a write replaced). Empty stops every time."));
    connect(m_bpType, &QComboBox::currentIndexChanged, this, [this](int type) { bpTypeChanged(type); });
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *clearHits = new QPushButton(QStringLiteral("Clear Hits"));
    auto *clear = new QPushButton(QStringLiteral("Clear All"));
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    connect(clearHits, &QPushButton::clicked, this, [this] {
        msdosdebug_bp_clear_hits(m_dbg);
        resync();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        msdosdebug_bp_clear(m_dbg);
        resync();
    });
    for (QLineEdit *edit : { m_bpStart, m_bpEnd, m_bpAh, m_bpCond })
        connect(edit, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpAh);
    row->addWidget(m_bpCond, 1);
    row->addWidget(add);
    row->addWidget(clearHits);
    row->addWidget(clear);
    layout->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    layout->addWidget(m_bpMsg);
    m_bpStop = new QLabel;
    m_bpStop->setStyleSheet(kGray);
    layout->addWidget(m_bpStop);
    return page;
}

QWidget *DebuggerWindow::buildWatches()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QHBoxLayout;
    form->addWidget(new QLabel(QStringLiteral("Watch")));
    m_wtEntry = new QLineEdit;
    m_wtEntry->setPlaceholderText(QStringLiteral("address, symbol, expression or first-last"));
    connect(m_wtEntry, &QLineEdit::returnPressed, this, [this] { addWatch(); });
    form->addWidget(m_wtEntry, 1);
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addWatch(); });
    form->addWidget(add);
    layout->addLayout(form);

    m_wtList = new QListWidget;
    setMono(m_wtList);
    layout->addWidget(m_wtList, 1);

    auto *foot = new QHBoxLayout;
    foot->addStretch();
    auto *mem = new QPushButton(QStringLiteral("Memory"));
    mem->setToolTip(QStringLiteral("Show the selected watch in the Memory tab"));
    connect(mem, &QPushButton::clicked, this, [this] {
        uint32_t first, last;
        if (m_wtList->currentRow() >= 0 && msdoswatch_range(m_dbg, m_wtList->currentRow(), &first, &last)) {
            showTab(TAB_MEMORY);
            showInMemory(first, last);
        }
    });
    auto *brk = new QPushButton(QStringLiteral("Break on Change"));
    brk->setToolTip(QStringLiteral("A memory write breakpoint on it with the condition value != old"));
    connect(brk, &QPushButton::clicked, this, [this] {
        uint32_t first, last;
        if (m_wtList->currentRow() < 0 || !msdoswatch_range(m_dbg, m_wtList->currentRow(), &first, &last)) return;
        char err[200] = "";
        if (msdosdebug_bp_add_if(m_dbg, MSDOS_BP_WRITE, first, last, -1, "value != old", err, sizeof err) >= 0)
            message(QStringLiteral("Breaking when it changes"));
        else
            message(err[0] ? cleanText(err) : QStringLiteral("The breakpoint could not be added"));
        syncLater();
    });
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_wtList->currentRow() >= 0) {
            msdoswatch_remove(m_wtList->currentRow());
            syncLater();
        }
    });
    foot->addWidget(mem);
    foot->addWidget(brk);
    foot->addWidget(remove);
    layout->addLayout(foot);
    return page;
}

/* The memory map above, the machine below (CPU, interrupt controller, timer, DMA,
 * ports, disks, cards). */
QWidget *DebuggerWindow::buildMachine()
{
    m_mapText = monoView();
    m_ioText = monoView();
    m_mapText->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(m_mapText);
    split->addWidget(m_ioText);
    split->setChildrenCollapsible(false);
    split->setSizes({ 520, 600 });
    return split;
}

/* Trace (0) and History (1): a Record switch, a filter, a note and the text.
 * Trace follows the machine live; History is what ran before the stop. */
QWidget *DebuggerWindow::buildTrace(int which)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;

    m_hRec[which] = new QPushButton(QStringLiteral("Record"));
    m_hRec[which]->setCheckable(true);
    m_hRec[which]->setStyleSheet(QStringLiteral("QPushButton:checked { background: %1; color: %2; }")
                                     .arg(msdosDebugAccentFill(m_hRec[which]).name(), msdosDebugAccentFillInk(m_hRec[which]).name()));
    m_hRec[which]->setToolTip(QStringLiteral("Record every instruction the CPU runs (the PC runs slower while recording)"));
    connect(m_hRec[which], &QPushButton::toggled, this, [this, which](bool enabled) {
        if (m_hSync) return;
        if (which == 0) msdosdebug_trace_enable(m_dbg, enabled);
        else msdosdebug_history_enable(m_dbg, enabled);
        syncLater();
    });
    m_hFilter[which] = new QComboBox;
    m_hFilter[which]->addItems({ QStringLiteral("All instructions"), QStringLiteral("Branches only"),
                                 QStringLiteral("In range") });
    m_hRange[which] = new QLineEdit;
    m_hRange[which]->setPlaceholderText(QStringLiteral("range: lo-hi or a symbol"));
    auto changed = [this] {
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        syncLater();
    };
    connect(m_hFilter[which], &QComboBox::currentIndexChanged, this, changed);
    connect(m_hRange[which], &QLineEdit::returnPressed, this, changed);
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this, which] {
        if (which == 0) msdosdebug_trace_clear(m_dbg);
        else msdosdebug_history_clear(m_dbg);
        m_hSeen[0] = m_hSeen[1] = ~0ull;
        syncLater();
    });
    m_hNote[which] = new QLabel;
    m_hNote[which]->setStyleSheet(kGray);
    m_hNote[which]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bar->addWidget(m_hRec[which]);
    bar->addWidget(m_hFilter[which]);
    bar->addWidget(m_hRange[which], 1);
    bar->addWidget(clear);
    bar->addWidget(m_hNote[which], 1);
    layout->addLayout(bar);
    m_hText[which] = monoView();
    layout->addWidget(m_hText[which], 1);
    return page;
}

QWidget *DebuggerWindow::buildSound()
{
    m_soundText = monoView();
    return m_soundText;
}

/* Symbols, from a linker map at a load segment. */
QWidget *DebuggerWindow::buildSymbols()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *bar = new QHBoxLayout;
    bar->addWidget(new QLabel(QStringLiteral("Filter")));
    m_symFilter = new QLineEdit;
    connect(m_symFilter, &QLineEdit::textChanged, this, [this] { refreshSymbols(false); });
    bar->addWidget(m_symFilter, 1);
    m_symCount = new QLabel;
    bar->addWidget(m_symCount);
    layout->addLayout(bar);
    m_symList = new QListWidget;
    setMono(m_symList);
    m_symList->setToolTip(QStringLiteral("Enter or double-click shows it in the Disassembly"));
    connect(m_symList, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        showTab(TAB_DISASM);
        showInDisasm(item->data(Qt::UserRole).toUInt());
    });
    layout->addWidget(m_symList, 1);

    auto *load = new QHBoxLayout;
    load->addWidget(new QLabel(QStringLiteral("Load segment")));
    m_symSeg = smallEntry(QStringLiteral("0000"), 6);
    m_symSeg->setToolTip(QStringLiteral("The segment the program was loaded at: a .COM's PSP, an .EXE's PSP + 10h"));
    load->addWidget(m_symSeg);
    auto *loadBtn = new QPushButton(QStringLiteral("Load Symbols…"));
    loadBtn->setToolTip(QStringLiteral("Labels from an MS LINK or Watcom WLINK .map file"));
    connect(loadBtn, &QPushButton::clicked, this, [this] { loadSymbols(); });
    auto *clearBtn = new QPushButton(QStringLiteral("Clear Symbols"));
    connect(clearBtn, &QPushButton::clicked, this, [this] {
        msdosdebug_clear_symbols(m_dbg);
        say(QStringLiteral("Symbols cleared"));
        refreshSymbols(true);
        resync();
    });
    load->addWidget(loadBtn);
    load->addWidget(clearBtn);
    load->addStretch();
    auto *disasm = new QPushButton(QStringLiteral("Disassembly"));
    disasm->setToolTip(QStringLiteral("Show the selected symbol in the Disassembly"));
    connect(disasm, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *item = m_symList->currentItem()) emit m_symList->itemActivated(item);
    });
    auto *memory = new QPushButton(QStringLiteral("Memory"));
    memory->setToolTip(QStringLiteral("Show the selected symbol in the Memory tab"));
    connect(memory, &QPushButton::clicked, this, [this] {
        if (QListWidgetItem *item = m_symList->currentItem()) {
            const uint32_t addr = item->data(Qt::UserRole).toUInt();
            showTab(TAB_MEMORY);
            showInMemory(addr, addr);
        }
    });
    auto *breakAt = new QPushButton(QStringLiteral("Break at"));
    breakAt->setToolTip(QStringLiteral("Toggle an execute breakpoint at the selected symbol"));
    connect(breakAt, &QPushButton::clicked, this, [this] { breakAtSymbol(); });
    load->addWidget(disasm);
    load->addWidget(memory);
    load->addWidget(breakAt);
    layout->addLayout(load);
    return page;
}

QWidget *DebuggerWindow::buildInts()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    auto *row = new QHBoxLayout;
    row->addWidget(titleLabel(QStringLiteral("INT calls")), 1);
    m_intOn = new QCheckBox(QStringLiteral("Trace"));
    m_intOn->setToolTip(QStringLiteral("Record every INT the PC runs (costs speed)"));
    connect(m_intOn, &QCheckBox::clicked, this, [this](bool enabled) {
        msdosdebug_int_trace_enable(m_dbg, enabled);
        refreshInts(true);
    });
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        msdosdebug_int_trace_clear(m_dbg);
        refreshInts(true);
    });
    row->addWidget(m_intOn);
    row->addWidget(clear);
    leftLayout->addLayout(row);
    m_intText = monoView();
    leftLayout->addWidget(m_intText, 1);
    left->setMinimumWidth(520);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->addWidget(titleLabel(QStringLiteral("Vector table")));
    m_ivtText = monoView();
    /* four vectors a row, as they are laid out */
    m_ivtText->setMinimumWidth(m_ivtText->fontMetrics().horizontalAdvance(
                                   QStringLiteral("00 0000:0000   01 0000:0000   02 0000:0000   03 0000:0000"))
                               + m_ivtText->verticalScrollBar()->sizeHint().width()
                               + 2 * (m_ivtText->frameWidth() + int(m_ivtText->document()->documentMargin())) + 4);
    rightLayout->addWidget(m_ivtText, 1);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setChildrenCollapsible(false);
    return split;
}

QWidget *DebuggerWindow::buildFujinet()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = hintLabel(QString());
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        msdosdebug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    leftLayout->addLayout(row);
    m_fujiText = monoView();
    leftLayout->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->addWidget(titleLabel(QStringLiteral("FujiNet's UART")));
    m_uartText = monoView();
    rightLayout->addWidget(m_uartText, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus(const msdos_stop_snapshot &snap)
{
    const bool running = msdossession_is_running(m_session) != 0;
    const bool stopped = running && snap.paused != 0;
    QString text;
    if (!running) text = QStringLiteral("The PC is not running");
    else if (stopped) text = QStringLiteral("Stopped: %1").arg(cleanText(snap.text));
    else text = QStringLiteral("Running");
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
        m_status->update();
    }
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    /* the action on offer wears the accent: Run while stopped */
    m_runBtn->setStyleSheet(stopped ? QStringLiteral("background: %1; color: %2;")
                                          .arg(msdosDebugAccentFill(m_runBtn).name(), msdosDebugAccentFillInk(m_runBtn).name())
                                    : QString());
}

/* The refresh never overwrites a value being typed. */
void DebuggerWindow::setEntry(QLineEdit *edit, const QString &text)
{
    if (edit->hasFocus()) return;
    if (edit->text() != text) edit->setText(text);
}

/* The general registers across one row; on a 386, with eight digits each, over two (the
 * window would grow too wide for a laptop screen otherwise). */
void DebuggerWindow::layoutRegs(bool wide)
{
    for (int idx = 0; idx < NREGS; ++idx) {
        m_regGrid->removeWidget(m_regCell[idx]);
        const int perRow = wide ? 5 : NREGS;
        m_regGrid->addWidget(m_regCell[idx], idx / perRow, idx % perRow);
    }
}

void DebuggerWindow::refreshRegs()
{
    msdos_cpu_regs regs;
    if (msdosdebug_regs(m_dbg, &regs) != 0) return;
    const uint32_t vals[NREGS] = { regs.eax, regs.ebx, regs.ecx, regs.edx, regs.esi, regs.edi, regs.ebp, regs.esp, regs.eip };
    const uint16_t svals[NSREGS] = { regs.cs, regs.ds, regs.es, regs.ss, regs.fs, regs.gs };
    const uint32_t bases[NSREGS] = { regs.cs_base, regs.ds_base, regs.es_base, regs.ss_base, regs.fs_base, regs.gs_base };
    if (m_is386 != (regs.is386 != 0)) {
        /* the entries as wide as the registers: 8 digits only from the 386 */
        m_is386 = regs.is386 != 0;
        for (QLineEdit *edit : m_reg) setDigits(edit, m_is386 ? 8 : 4);
        layoutRegs(m_is386);
    }
    for (int idx = 0; idx < NREGS; ++idx) {
        m_regLabel[idx]->setText(QString::fromLatin1(m_is386 ? kRegs32[idx] : kRegs16[idx]).toUpper());
        setEntry(m_reg[idx], m_is386 ? hexText(vals[idx], 8) : hexText(vals[idx] & 0xFFFFu, 4));
    }
    for (int idx = 0; idx < NSREGS; ++idx) {
        const bool shown = idx < 4 || m_is386;
        m_sregCell[idx]->setVisible(shown);
        m_sbase[idx]->setVisible(shown);
        setEntry(m_sreg[idx], hexText(svals[idx], 4));
        m_sbase[idx]->setText(QStringLiteral("base %1").arg(hexText(bases[idx], 5)));
    }
    for (int idx = 0; idx < NFLAGS; ++idx) m_flag[idx]->setChecked((regs.eflags >> kFlags[idx].bit) & 1);
    static const char *const modes[] = { "real mode", "protected mode", "virtual 8086 mode" };
    char video[160] = "";
    msdosdebug_video_info(m_dbg, video, sizeof video);
    const QString vid = QString::fromUtf8(video).trimmed();
    QString info;
    if (regs.is386)
        info = QStringLiteral("%1   %2   CR0 %3   %4").arg(QString::fromUtf8(regs.cpu), QLatin1String(modes[qBound(0, regs.mode, 2)]),
                                                         hexText(regs.cr0, 8), vid);
    else
        info = QStringLiteral("%1   %2").arg(QString::fromUtf8(regs.cpu), vid);
    if (m_cpuInfo->text() != info) {
        m_cpuInfo->setText(info);
        m_cpuInfo->setToolTip(info);
        m_cpuInfo->update();
    }
}

/* The memory map labels the Memory rows: read for the stop that is current. */
void DebuggerWindow::refreshRegions()
{
    msdos_stop_snapshot snap;
    msdosdebug_stop_snapshot(m_dbg, &snap);
    if (!m_regions.empty() && m_regionSerial == snap.serial && snap.paused) return;
    msdos_region map[REGION_MAX];
    const int count = msdosdebug_memory_map(m_dbg, map, REGION_MAX);
    m_regions.assign(map, map + qMax(0, count));
    m_regionSerial = snap.serial;
}

/* The dump from m_memTop, each row labelled once with the region it is in (the
 * map as of the last stop); a highlighted range (Go to, a watch, a search hit)
 * in the accent. */
void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_PAGE];
    memset(mem, 0xFF, sizeof mem);
    msdosdebug_read(m_dbg, m_memTop, mem, int(sizeof mem));

    /* five hex digits cover the first megabyte; a page beyond it needs eight */
    const int digits = quint64(m_memTop) + MEM_PAGE > 0x100000ull ? 8 : 5;
    m_memPrefix = digits + 2;
    QString text;
    text.reserve(MEM_ROWS * 90);
    text += QString(m_memPrefix, QLatin1Char(' '));
    for (int col = 0; col < 16; ++col) text += hexText(quint64(col), 1) + QStringLiteral("  ");
    char region[64];
    QString lastRegion;
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *bytes = mem + row * 16;
        const quint64 addr = quint64(m_memTop) + quint64(row) * 16;
        text += QStringLiteral("\n%1: ").arg(hexText(addr, digits));
        for (int col = 0; col < 16; ++col) text += hexText(bytes[col], 2) + QLatin1Char(' ');
        text += QLatin1Char(' ');
        for (int col = 0; col < 16; ++col) text += QLatin1Char(bytes[col] >= 0x20 && bytes[col] < 0x7F ? char(bytes[col]) : '.');
        if (!m_regions.empty() &&
            msdosmem_region_name(m_regions.data(), int(m_regions.size()), uint32_t(addr), region, sizeof region) > 0) {
            const QString name = QString::fromUtf8(region);
            if (name != lastRegion) text += QStringLiteral("  ; ") + name;
            lastRegion = name;
        }
    }
    setText(m_mem, cleanText(text));

    /* the bytes Go to found stay marked across refreshes */
    QList<QTextEdit::ExtraSelection> sel;
    if (m_memHl) {
        for (int row = 0; row < MEM_ROWS; ++row) {
            const QTextBlock block = m_mem->document()->findBlockByNumber(1 + row);
            if (!block.isValid()) break;
            for (int col = 0; col < 16; ++col) {
                const quint64 addr = quint64(m_memTop) + quint64(row) * 16 + quint64(col);
                if (addr < m_memHlLo || addr > m_memHlHi) continue;
                QTextEdit::ExtraSelection mark;
                mark.cursor = QTextCursor(m_mem->document());
                mark.cursor.setPosition(block.position() + m_memPrefix + col * 3);
                mark.cursor.setPosition(block.position() + m_memPrefix + col * 3 + 2, QTextCursor::KeepAnchor);
                mark.format = accentFormat(m_mem);
                sel << mark;
            }
        }
    }
    m_mem->setExtraSelections(sel);
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * int(m_disasm->document()->documentMargin());
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

/* x86 code cannot be read backwards: the offset rows instructions before off
 * is the nearest start whose instructions land on off. */
uint32_t DebuggerWindow::rowsBack(uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW * 4];
    const int count = qMin(rows * 7 + 1, int(sizeof lines / sizeof lines[0]));
    for (int back = rows; back <= rows * 7 && uint32_t(back) <= off; back++) {
        const int found = msdosdebug_disassemble(m_dbg, seg, off - uint32_t(back), count, lines);
        int idx = 0;
        while (idx < found && lines[idx].off < off) idx++;
        if (idx < found && lines[idx].off == off && idx >= rows) return lines[idx - rows].off;
    }
    return off > uint32_t(rows) * 3 ? off - uint32_t(rows) * 3 : 0;
}

uint32_t DebuggerWindow::rowsOn(uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW + 1];
    const int found = msdosdebug_disassemble(m_dbg, seg, off, qMin(rows + 1, DISASM_WINDOW + 1), lines);
    return found > rows ? lines[rows].off : off;
}

void DebuggerWindow::refreshDisasm()
{
    static msdos_dis_line lines[DISASM_WINDOW];
    static msdos_breakpoint list[BP_MAX];
    int bps[DISASM_WINDOW];
    const int rows = visibleRows();
    const int listed = msdosdebug_bp_list(m_dbg, list, BP_MAX);
    msdos_cpu_regs regs;
    const bool haveRegs = msdosdebug_regs(m_dbg, &regs) == 0;

    /* Following, CS:IP stays put while it shows, else sits a third of the
     * way down. */
    if (haveRegs && m_followPc->isChecked()) {
        const uint32_t pcLinear = msdosdebug_linear(m_dbg, regs.cs, regs.eip);
        bool shows = false;
        for (int idx = 1; idx < int(m_lineLin.size()) && idx < rows - 2; ++idx)
            if (m_lineLin[size_t(idx)] == pcLinear) shows = true;
        if (!shows || m_topSeg != regs.cs) {
            m_topSeg = regs.cs;
            m_topOff = rowsBack(regs.cs, regs.eip, rows / 3);
        }
    }

    const int count = msdosdebug_disassemble(m_dbg, m_topSeg, m_topOff, qMin(rows + 1, DISASM_WINDOW), lines);
    int pcLine = -1;
    m_lineLin.clear();
    QString text;
    for (int row = 0; row < count; ++row) {
        const msdos_dis_line *line = &lines[row];
        const bool isPc = haveRegs && line->seg == regs.cs && line->off == regs.eip;
        /* the gutter: 1 an execute breakpoint here, 2 one that is off */
        bps[row] = 0;
        for (int found = 0; found < listed; ++found)
            if (list[found].type == MSDOS_BP_EXEC && list[found].start == line->linear && list[found].end == line->linear &&
                bps[row] != 1)
                bps[row] = list[found].enabled ? 1 : 2;
        m_lineLin.push_back(line->linear);
        if (isPc) pcLine = row;
        if (row) text += QLatin1Char('\n');
        /* the gutter: a breakpoint dot, then the PC's arrow */
        text += bps[row] ? QStringLiteral("●") : QStringLiteral(" ");
        appendf(text, "%c %04X:%0*X  %-20s %s%s%s", isPc ? '>' : ' ', line->seg, line->off > 0xFFFF ? 8 : 4, line->off,
                line->bytes, line->symbol, line->symbol[0] ? ": " : "", line->text);
    }
    if (count == 0) text = QStringLiteral("(no disassembly)");
    m_disasm->setPlainText(cleanText(text));
    m_disasm->verticalScrollBar()->setValue(0);

    QList<QTextEdit::ExtraSelection> sel;
    for (int row = 0; row < count; ++row) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (bps[row]) {
            QTextEdit::ExtraSelection mark;
            mark.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(row));
            mark.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            if (bps[row] == 2) {
                mark.format.setForeground(QColor(0x9a, 0x99, 0x96));
            } else {
                mark.format.setForeground(QColor(0xe0, 0x1b, 0x24));
                mark.format.setFontWeight(QFont::Bold);
            }
            sel << mark;
        }
        /* the selected line underlined */
        if (int64_t(lines[row].linear) == m_selected && row != pcLine) {
            QTextEdit::ExtraSelection mark;
            mark.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(row));
            mark.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            mark.format.setFontUnderline(true);
            mark.format.setFontWeight(QFont::Bold);
            sel << mark;
        }
    }
    /* the CS:IP line in the accent colour, last so its ink wins */
    if (pcLine >= 0) {
        QTextCharFormat pcFormat = accentFormat(m_disasm);
        if (int64_t(lines[pcLine].linear) == m_selected) pcFormat.setFontUnderline(true);
        sel << lineSelection(m_disasm, pcLine, pcFormat);
    }
    m_disasm->setExtraSelections(sel);
}

void DebuggerWindow::refreshBps()
{
    static msdos_breakpoint bps[BP_MAX];
    const int count = msdosdebug_bp_list(m_dbg, bps, BP_MAX);

    /* what stopped the machine, in the core's words */
    msdos_stop_snapshot snap;
    msdosdebug_stop_snapshot(m_dbg, &snap);
    m_bpStop->setText(snap.paused ? QStringLiteral("Stopped: %1").arg(cleanText(snap.text)) : QStringLiteral("Running."));

    /* rebuilt only when it changed (each step refreshes), and never while a
     * row's own button is being handled */
    QString sig;
    for (int idx = 0; idx < count; ++idx)
        appendf(sig, "%d %d %u %s %s\n", bps[idx].id, bps[idx].enabled, bps[idx].hits, bps[idx].text, bps[idx].condition);
    if (sig == m_bpSig && m_bpTable->rowCount() == count) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(count);
    for (int idx = 0; idx < count; ++idx) {
        auto *enabledItem = new QTableWidgetItem;
        enabledItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        enabledItem->setCheckState(bps[idx].enabled ? Qt::Checked : Qt::Unchecked);
        enabledItem->setData(Qt::UserRole, bps[idx].id);
        enabledItem->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(idx, 0, enabledItem);
        m_bpTable->setItem(idx, 1, new QTableWidgetItem(QStringLiteral("#%1").arg(bps[idx].id)));
        auto *what = new QTableWidgetItem(cleanText(bps[idx].text));
        char desc[256];
        msdosbp_describe(m_dbg, &bps[idx], desc, sizeof desc);
        what->setToolTip(cleanText(desc));
        m_bpTable->setItem(idx, 2, what);
        auto *cond = new QTableWidgetItem(cleanText(bps[idx].condition));
        cond->setData(Qt::UserRole, bps[idx].id);
        cond->setToolTip(QStringLiteral("Double-click to change the condition (empty: stop every time)"));
        m_bpTable->setItem(idx, 3, cond);
        m_bpTable->setItem(idx, 4, new QTableWidgetItem(QString::number(bps[idx].hits)));
        for (int col = 1; col < 5; ++col) {
            m_bpTable->item(idx, col)->setFont(mono);
            if (col != 3) m_bpTable->item(idx, col)->setFlags(Qt::ItemIsEnabled);
        }
        auto *removeBtn = new QToolButton;
        removeBtn->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (removeBtn->icon().isNull()) removeBtn->setText(QStringLiteral("Remove"));
        removeBtn->setAutoRaise(true);
        removeBtn->setToolTip(QStringLiteral("Remove"));
        const int bpid = bps[idx].id;
        connect(removeBtn, &QToolButton::clicked, this, [this, bpid] {
            msdosdebug_bp_remove(m_dbg, bpid);
            syncLater();
        });
        m_bpTable->setCellWidget(idx, 5, removeBtn);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(count == 0);
}

void DebuggerWindow::refreshWatches()
{
    const int count = msdoswatch_count();
    const int keep = m_wtList->currentRow();
    QStringList rows;
    for (int idx = 0; idx < count; ++idx) {
        char line[256];
        msdoswatch_format(m_dbg, idx, line, sizeof line);
        rows << cleanText(line);
    }
    /* unchanged: leave the list alone (a click in it must not be lost) */
    bool same = m_wtList->count() == count;
    for (int idx = 0; same && idx < count; ++idx) same = m_wtList->item(idx)->text() == rows[idx];
    if (same) return;
    m_wtList->clear();
    m_wtList->addItems(rows);
    if (keep >= 0 && keep < count) m_wtList->setCurrentRow(keep);
}

/* The machine and its memory map: from the engine's getters (valid while
 * stopped). */
void DebuggerWindow::refreshMachine()
{
    static char text[16384];
    msdos_region map[REGION_MAX];
    QString top;
    const int regions = msdosdebug_memory_map(m_dbg, map, REGION_MAX);
    top += QStringLiteral("Memory map (linear addresses)\n");
    if (!regions) top += QStringLiteral("  The PC is off.\n");
    for (int idx = 0; idx < regions; ++idx)
        top += QStringLiteral("  %1-%2  %3%4  %5\n")
                   .arg(hexText(map[idx].low, 8), hexText(map[idx].high, 8))
                   .arg(map[idx].readable ? QLatin1Char('r') : QLatin1Char('-'))
                   .arg(map[idx].writable ? QLatin1Char('w') : QLatin1Char('-'))
                   .arg(QString::fromUtf8(map[idx].name));
    setText(m_mapText, cleanText(top));
    text[0] = '\0';
    msdosdebug_io_text(m_dbg, text, int(sizeof text));
    setText(m_ioText, text[0] ? cleanText(text) : QStringLiteral("The PC is off."));
}

/* Trace (which 0: the newest instructions, live) and History (1: what ran
 * before the stop). Both read the one ring; the filter and the note are the
 * tab's own. */
void DebuggerWindow::refreshTrace(int which, bool force)
{
    static msdos_trace_entry buf[HIST_FETCH];
    const bool live = which == 0;
    const bool stopped = msdosdebug_stopped(m_dbg) != 0;
    const bool traceOn = msdosdebug_trace_enabled(m_dbg) != 0, histOn = msdosdebug_history_enabled(m_dbg) != 0;
    const bool recording = live ? traceOn : histOn;
    const uint64_t total = live ? msdosdebug_trace_total(m_dbg) : msdosdebug_history_count(m_dbg);
    uint32_t first = 0, last = 0xFFFFFFFFu;
    int filter = MSDOS_TRACE_ALL;

    m_hSync = true;      /* the two Record buttons follow the engine */
    m_hRec[0]->setChecked(traceOn);
    m_hRec[1]->setChecked(histOn);
    m_hSync = false;

    if (!force && total == m_hSeen[which] && (live || stopped)) return;
    m_hSeen[which] = total;
    if (!live && !stopped) {
        m_hNote[which]->setText(QStringLiteral("Stop the PC to read its history."));
        return;
    }
    switch (m_hFilter[which]->currentIndex()) {
    case 1: filter = MSDOS_TRACE_BRANCHES; break;
    case 2:
        if (msdossym_parse_range(m_dbg, m_hRange[which]->text().toUtf8().constData(), &first, &last)) filter = MSDOS_TRACE_RANGE;
        break;
    default: break;
    }
    QList<int> hit;
    int fetched = 0;
    if (live) {
        fetched = msdosdebug_trace_read(m_dbg, buf, HIST_FETCH);                /* newest first */
        for (int idx = 0; idx < fetched && hit.size() < HIST_SHOWN; ++idx)
            if (msdosdebug_trace_match(&buf[idx], filter, first, last)) hit.append(idx);
    } else {
        fetched = msdosdebug_history_copy(m_dbg, buf, HIST_FETCH);              /* oldest first */
        for (int idx = fetched - 1; idx >= 0 && hit.size() < HIST_SHOWN; --idx)   /* the newest matches, scanning back */
            if (msdosdebug_trace_match(&buf[idx], filter, first, last)) hit.append(idx);
    }
    QString text;
    if (!recording && fetched == 0)
        text = QStringLiteral("%1 is off. Press Record, then let the PC run.\n").arg(live ? QStringLiteral("Trace") : QStringLiteral("History"));
    for (int pos = hit.size() - 1; pos >= 0; --pos) {      /* oldest first */
        char line[400];
        msdosdebug_trace_format(&buf[hit[pos]], line, sizeof line);
        text += cleanText(line);
        text += QLatin1Char('\n');
    }
    setText(m_hText[which], text, true);
    m_hNote[which]->setText(QStringLiteral("%1%2 instructions recorded, last %3 shown")
                                .arg(recording ? QStringLiteral("Recording. ") : QString()).arg(total).arg(hit.size()));
}

/* The PC speaker and the FM chip, from the engine's mirrors (stopped only). */
void DebuggerWindow::refreshSound()
{
    static char text[16384];
    if (!msdosdebug_stopped(m_dbg)) {
        setText(m_soundText, QStringLiteral("Stop the PC to read the sound devices."));
        return;
    }
    text[0] = '\0';
    msdosdebug_sound_text(m_dbg, text, int(sizeof text));
    setText(m_soundText, text[0] ? cleanText(text) : QStringLiteral("The PC is off."));
}

/* Only rebuilt when the list or the filter changed: a real program's map can be
 * a few hundred entries. force: whatever the tab shows, on first open too. */
void DebuggerWindow::refreshSymbols(bool force)
{
    const unsigned generation = msdosdebug_symbols_generation(m_dbg);
    const QString filter = m_symFilter->text();
    if (!force && generation == m_symGen && filter == m_symShownFilter) return;
    m_symGen = generation;
    m_symShownFilter = filter;
    const int total = msdosdebug_symbols_count(m_dbg);
    std::vector<int> found(size_t(qMax(total, 1)));
    const int shown = msdosdebug_symbols_filter(m_dbg, filter.toUtf8().constData(), found.data(), int(found.size()));
    m_symList->clear();
    for (int idx = 0; idx < shown; ++idx) {
        uint32_t addr;
        char name[96];
        if (!msdosdebug_symbols_get(m_dbg, found[size_t(idx)], &addr, name, sizeof name)) continue;
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(hexText(addr, 5), QString::fromUtf8(name)));
        item->setData(Qt::UserRole, addr);
        m_symList->addItem(item);
    }
    m_symCount->setText(QStringLiteral("%1 symbol%2%3").arg(shown).arg(shown == 1 ? QString() : QStringLiteral("s"))
                            .arg(filter.isEmpty() ? QString() : QStringLiteral(" (filtered)")));
}

void DebuggerWindow::refreshInts(bool force)
{
    static msdos_int_event events[TRACE_MAX];
    const int count = msdosdebug_int_trace(m_dbg, events, TRACE_MAX);
    m_intOn->setChecked(msdosdebug_int_trace_enabled(m_dbg) != 0);

    /* the vector table: changes rarely, cheap to read */
    QString table;
    for (int vec = 0; vec < 256; ++vec) {
        uint16_t seg = 0, off = 0;
        msdosdebug_ivt(m_dbg, vec, &seg, &off);
        appendf(table, "%02X %04X:%04X%s", vec, seg, off, (vec & 3) == 3 ? (vec < 255 ? "\n" : "") : "   ");
    }
    setText(m_ivtText, table);

    /* a log: set again only when it moved */
    if (!force && count == m_intCount && (count == 0 || events[count - 1].serial == m_intLast)) return;
    m_intCount = count;
    m_intLast = count > 0 ? events[count - 1].serial : 0;
    QString log = QStringLiteral("From       AX   BX   CX   DX   DS   ES    Call");
    for (int idx = 0; idx < count; ++idx)
        appendf(log, "\n%04X:%04X  %04X %04X %04X %04X %04X %04X  %s", events[idx].cs, events[idx].ip & 0xFFFF, events[idx].ax,
                events[idx].bx, events[idx].cx, events[idx].dx, events[idx].ds, events[idx].es, events[idx].text);
    if (count == 0)
        log += msdosdebug_int_trace_enabled(m_dbg) ? QStringLiteral("\n(no INT calls since the trace was cleared)")
                                                   : QStringLiteral("\n(turn the trace on to record INT calls; it costs speed)");
    setText(m_intText, cleanText(log), force || atEnd(m_intText));
}

void DebuggerWindow::refreshFujinet(bool force)
{
    static msdos_fuji_packet packets[TRACE_MAX];
    static char uart[2048];
    const int count = msdosdebug_fujinet_trace(m_dbg, packets, TRACE_MAX);
    const bool external = msdossession_fujinet_external(m_session) != 0;
    if (!msdossession_fujinet_running(m_session) && !external)
        m_fujiNote->setText(QStringLiteral("FujiNet is off (Preferences ▸ FujiNet)."));
    else if (external)
        m_fujiNote->setText(QStringLiteral("External FujiNet at %1 on COM%2%3.")
                                .arg(QString::fromUtf8(msdossession_boip_endpoint(m_session)))
                                .arg(msdossession_fujinet_com(m_session))
                                .arg(msdossession_fujinet_link(m_session) ? QStringLiteral(", connected")
                                                                          : QStringLiteral(", not connected")));
    else
        m_fujiNote->setText(QStringLiteral("FujiNet is on COM%1%2.").arg(msdossession_fujinet_com(m_session))
                                .arg(msdossession_fujinet_link(m_session) ? QStringLiteral(", connected")
                                                                          : QStringLiteral(", not connected")));
    uart[0] = '\0';
    msdosdebug_uart(m_dbg, uart, sizeof uart);
    setText(m_uartText, cleanText(uart));

    if (!force && count == m_fujiCount && (count == 0 || packets[count - 1].serial == m_fujiLast)) return;
    m_fujiCount = count;
    m_fujiLast = count > 0 ? packets[count - 1].serial : 0;
    QString text;
    for (int idx = 0; idx < count; ++idx) {
        appendf(text, "%s%s\n     ", idx ? "\n" : "", packets[idx].text);
        for (int byte = 0; byte < packets[idx].nbytes && byte < int(sizeof packets[idx].data); byte++)
            appendf(text, "%02X ", packets[idx].data[byte]);
        if (packets[idx].nbytes > int(sizeof packets[idx].data)) text += QStringLiteral("…");
    }
    if (count == 0) text = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, cleanText(text), force || atEnd(m_fujiText));
}

/* One tab; live is the twice-a-second refresh while the machine runs, which
 * leaves out what only changes at a stop. */
void DebuggerWindow::refreshTab(int tab, bool live)
{
    /* with the PC off only what needs no machine is shown: breakpoints, watches and symbols */
    if (!msdossession_is_running(m_session) && tab != TAB_BREAKS && tab != TAB_WATCHES && tab != TAB_SYMBOLS) return;
    switch (tab) {
    case TAB_DISASM: if (!live) refreshDisasm(); break;
    case TAB_MEMORY: if (!live) refreshRegions(); refreshMem(); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_WATCHES: refreshWatches(); break;
    case TAB_MACHINE: if (!live) refreshMachine(); break;
    case TAB_TRACE: refreshTrace(0, !live); break;
    case TAB_HISTORY: if (!live) refreshTrace(1, true); break;
    case TAB_VIDEO: m_video->refresh(!live); break;
    case TAB_SOUND: if (!live) refreshSound(); break;
    case TAB_SYMBOLS: refreshSymbols(false); break;     /* it fills when its generation first differs */
    case TAB_INTS: refreshInts(false); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

/* Every tab that is on screen (the one showing, and any peeled off). */
void DebuggerWindow::refreshAll()
{
    msdos_stop_snapshot snap;
    msdosdebug_stop_snapshot(m_dbg, &snap);
    refreshStatus(snap);
    if (msdossession_is_running(m_session)) refreshRegs();
    for (int tab = 0; tab < TAB_COUNT; ++tab)
        if (tabVisible(tab)) refreshTab(tab, false);
}

/* After anything that may have moved the engine: the console notes a new
 * stop (as DEBUG's R does), then the tabs showing follow. */
void DebuggerWindow::resync()
{
    msdos_stop_snapshot snap;
    msdosdebug_stop_snapshot(m_dbg, &snap);
    if (snap.paused && snap.serial != m_seenSerial && msdossession_is_running(m_session)) consoleStop(snap);
    m_wasStopped = snap.paused != 0;
    m_seenSerial = snap.serial;
    m_seenGen = msdosdebug_generation(m_dbg);
    m_runningTicks = 0;
    refreshAll();
}

/* From inside a breakpoint row's own signal: the table rebuilds after. */
void DebuggerWindow::syncLater()
{
    QTimer::singleShot(0, this, [this] { resync(); });
}

/* A step takes microseconds: a moment's wait for the stop keeps the window
 * from flashing "Running" in between. */
void DebuggerWindow::settleAndSync()
{
    for (int tries = 0; tries < 40 && !msdosdebug_stopped(m_dbg); ++tries) QThread::msleep(1);
    resync();
}

void DebuggerWindow::attachAndRefresh()
{
    msdosdebug_attach(m_dbg);   /* stops the machine */
    m_attachedRunning = msdossession_is_running(m_session) != 0;
    m_wasStopped = false;
    m_seenSerial = ~0ull;
    settleAndSync();
}

/* The window follows the engine through the stop snapshot (the stop callback
 * fires on the emulation thread), so this polls it: a refresh when it moved,
 * and the live values while the machine runs. */
void DebuggerWindow::tick()
{
    if (!isVisible() || m_closing) return;
    if (!msdossession_is_running(m_session)) {
        if (m_attachedRunning) {
            m_attachedRunning = false;
            msdos_stop_snapshot off;
            msdosdebug_stop_snapshot(m_dbg, &off);
            refreshStatus(off);
        }
        return;
    }
    /* Opened before the PC started, or showing across a restart: the engine
     * attaches (stopping the machine) as it comes up. */
    if (!m_attachedRunning) {
        msdosdebug_detach(m_dbg);
        attachAndRefresh();
        return;
    }
    msdos_stop_snapshot snap;
    msdosdebug_stop_snapshot(m_dbg, &snap);
    const unsigned gen = msdosdebug_generation(m_dbg);
    const bool stopped = snap.paused != 0;
    if (snap.serial != m_seenSerial || gen != m_seenGen || stopped != m_wasStopped) {
        resync();
        return;
    }
    if (stopped) return;
    ++m_runningTicks;
    /* the Video tab follows the beam: every tick, not twice a second */
    if (tabVisible(TAB_VIDEO)) m_video->refresh(false);
    if (m_runningTicks % 4 == 0 && tabVisible(TAB_TRACE)) refreshTrace(0, false);
    if (m_runningTicks >= 5) {
        m_runningTicks = 0;
        refreshStatus(snap);
        refreshRegs();
        for (int tab = 0; tab < TAB_COUNT; ++tab)
            if (tabVisible(tab) && tab != TAB_VIDEO && tab != TAB_TRACE) refreshTab(tab, true);
    }
}

/* ---- the console ---------------------------------------------------------- */

/* Onto the transcript, which follows its end while the reader is there. */
void DebuggerWindow::consoleAppend(const QString &text)
{
    const bool follow = atEnd(m_conOut);
    QTextCursor cur(m_conOut->document());
    cur.movePosition(QTextCursor::End);
    cur.insertText(text);
    if (follow) m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
}

/* Command output, ended with a newline. */
void DebuggerWindow::consoleOutput(const QString &text)
{
    const QString clean = cleanText(text);
    if (clean.isEmpty()) return;
    consoleAppend(clean.endsWith(QLatin1Char('\n')) ? clean : clean + QLatin1Char('\n'));
}

/* At a stop the console shows where, as DEBUG's R does. */
void DebuggerWindow::consoleStop(const msdos_stop_snapshot &snap)
{
    static char out[4096];
    if (snap.stop.reason != MSDOS_STOP_STEP && snap.text[0])
        consoleOutput(QStringLiteral("— ") + cleanText(snap.text));
    out[0] = '\0';
    msdosdebug_command(m_dbg, "r", out, sizeof out);
    consoleOutput(QString::fromUtf8(out));
}

void DebuggerWindow::runConsole()
{
    static char out[65536];
    const QString cmd = m_conIn->text().trimmed();
    if (cmd.isEmpty()) return;
    if (m_history.isEmpty() || m_history.last() != cmd) m_history << cmd;
    m_historyPos = int(m_history.size());
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QLatin1Char('-') + cmd + QLatin1Char('\n'));
    m_conIn->clear();

    out[0] = '\0';
    msdosdebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    consoleOutput(QString::fromUtf8(out));
    settleAndSync();
}

/* Tab: the engine completes the word at the end of the line. One match
 * replaces it; several are listed and the line extends to what they share. */
void DebuggerWindow::completeConsole()
{
    char comps[4096];
    const QString text = m_conIn->text();
    comps[0] = '\0';
    const int count = msdosdebug_completions(m_dbg, text.toUtf8().constData(), comps, sizeof comps);
    if (count <= 0) return;
    const QStringList words = cleanText(comps).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) return;
    const int space = text.lastIndexOf(QLatin1Char(' '));
    const QString head = text.left(space + 1);
    if (words.size() == 1) {
        m_conIn->setText(head + words[0] + QLatin1Char(' '));
        return;
    }
    QString common = words[0];
    for (const QString &word : words)
        while (!word.startsWith(common)) common.chop(1);
    consoleAppend(words.join(QStringLiteral("  ")) + QLatin1Char('\n'));
    if (common.size() > text.size() - space - 1) m_conIn->setText(head + common);
}

void DebuggerWindow::showHistory()
{
    m_conIn->setText(m_historyPos < int(m_history.size()) ? m_history.at(m_historyPos) : QString());
    m_conIn->end(false);
}

/* Messages land in the console and, since it may not be the tab showing,
 * the status bar. */
void DebuggerWindow::say(const QString &msg)
{
    consoleOutput(msg);
    message(msg);
}

void DebuggerWindow::message(const QString &msg)
{
    statusBar()->showMessage(msg, 6000);
}

/* ---- the rest of the handlers ----------------------------------------------- */

void DebuggerWindow::toggleRun()
{
    if (msdosdebug_stopped(m_dbg)) {
        msdosdebug_resume(m_dbg);
        resync();
    } else {
        msdosdebug_stop(m_dbg);
        settleAndSync();
    }
}

void DebuggerWindow::stepAnd(int (*func)(msdosdebug *))
{
    func(m_dbg);
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    msdosdebug_run_to(m_dbg, uint32_t(m_selected));
    settleAndSync();
}

void DebuggerWindow::toggleBreakpoint(uint32_t linear)
{
    if (msdosdebug_bp_toggle(m_dbg, linear) < 0)
        message(QStringLiteral("No room for another breakpoint"));
    resync();
}

void DebuggerWindow::toggleSelected()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly first"));
        return;
    }
    toggleBreakpoint(uint32_t(m_selected));
}

/* A number the way the prompt takes it: a bare token of hex digits is hex (as
 * the register and value entries always were), "#" decimal, anything else goes
 * to the engine's expression evaluator ($B800, 49152, a symbol, ax + 1 ...). */
bool DebuggerWindow::numberOf(const QString &text, int64_t *out, QString *why)
{
    static const QRegularExpression hexOnly(QStringLiteral("^[0-9A-Fa-f]{1,8}$"));
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (why) *why = QStringLiteral("Type a value");
        return false;
    }
    if (hexOnly.match(trimmed).hasMatch()) {
        *out = trimmed.toLongLong(nullptr, 16);
        return true;
    }
    bool decimal = false;
    if (trimmed.startsWith(QLatin1Char('#'))) {
        const qlonglong dec = trimmed.mid(1).toLongLong(&decimal, 10);
        if (decimal) {
            *out = dec;
            return true;
        }
    }
    char err[160] = "";
    if (msdosdebug_eval(m_dbg, trimmed.toUtf8().constData(), out, err, int(sizeof err)) == 0) return true;
    if (why) *why = cleanText(err);
    return false;
}

/* seg:off, a register pair, a linear address (hex), a symbol or an expression. */
bool DebuggerWindow::addressOf(const QString &text, uint32_t *linear)
{
    const QByteArray bytes = text.trimmed().toUtf8();
    if (bytes.isEmpty()) return false;
    uint16_t seg;
    uint32_t off, lin;
    if (msdosdebug_parse_address(m_dbg, bytes.constData(), &seg, &off, &lin) == 0) {
        *linear = lin;
        return true;
    }
    int64_t value;
    char err[160] = "";
    if (msdosdebug_eval(m_dbg, bytes.constData(), &value, err, int(sizeof err)) == 0 && value >= 0 && value <= 0xFFFFFFFFll) {
        *linear = uint32_t(value);
        return true;
    }
    return false;
}

bool DebuggerWindow::parseAddr(const QString &text, uint16_t *seg, uint32_t *off, uint32_t *lin)
{
    const QByteArray bytes = text.trimmed().toUtf8();
    if (bytes.isEmpty()) return false;
    return msdosdebug_parse_address(m_dbg, bytes.constData(), seg, off, lin) == 0;
}

/* One register or flag through the engine's grammar: it says if it will not
 * take the value (running, a segment in protected mode). */
void DebuggerWindow::applyRegister(const char *name, int64_t value, const QString &okMessage)
{
    char err[200] = "";
    if (msdosdebug_register_set(m_dbg, name, value, err, sizeof err))
        message(err[0] ? okMessage + QStringLiteral(": ") + cleanText(err) : okMessage);
    else
        message(err[0] ? cleanText(err) : QStringLiteral("The register could not be written"));
}

/* Edits go in only while the machine is stopped. */
void DebuggerWindow::setReg(int idx, bool segment)
{
    QLineEdit *edit = segment ? m_sreg[idx] : m_reg[idx];
    /* the 32-bit name when the 32-bit value shows */
    const char *name = segment ? kSregs[idx] : m_is386 ? kRegs32[idx] : kRegs16[idx];
    int64_t val = 0;
    QString why;
    if (!msdosdebug_stopped(m_dbg))
        message(QStringLiteral("Stop the PC first"));
    else if (!numberOf(edit->text(), &val, &why))
        message(why);
    else
        applyRegister(name, val, QStringLiteral("%1 set").arg(QString::fromLatin1(name).toUpper()));
    edit->clearFocus();
    resync();
}

void DebuggerWindow::setFlag(int idx, bool enabled)
{
    if (!msdosdebug_stopped(m_dbg)) message(QStringLiteral("Stop the PC first"));
    else applyRegister(kFlags[idx].name, enabled ? 1 : 0, QStringLiteral("Flag set"));
    resync();
}

/* Shows a range in the Memory tab, highlighted, on a row boundary and a
 * little above the middle. */
void DebuggerWindow::showInMemory(uint32_t first, uint32_t last)
{
    m_memHl = true;
    m_memHlLo = first;
    m_memHlHi = last;
    m_memTop = uint32_t(qMin<quint64>(quint64((first > 0x100 ? first - 0x100 : 0) & ~0xFu), 0xFFFFFFFFull - MEM_PAGE + 1)) & ~0xFu;
    refreshRegions();
    refreshMem();
    const int line = 1 + int((first - m_memTop) / 16);
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

/* Go to: an address, a symbol, an expression or lo-hi. */
void DebuggerWindow::memGoto()
{
    uint32_t first, last;
    if (msdossym_parse_range(m_dbg, m_memGoto->text().toUtf8().constData(), &first, &last)) {
        showInMemory(first, last);
    } else {
        m_memHl = false;
        refreshMem();
        message(QStringLiteral("Not an address (seg:off, ds:si, a linear address, a symbol, first-last)"));
    }
}

void DebuggerWindow::memPoke()
{
    uint32_t addr;
    int64_t val;
    QString why;
    if (!msdosdebug_stopped(m_dbg)) {
        message(QStringLiteral("Stop the PC first"));
        return;
    }
    if (!addressOf(m_memAddr->text(), &addr)) {
        message(QStringLiteral("Not an address"));
        return;
    }
    if (!numberOf(m_memVal->text(), &val, &why) || val < 0 || val > 0xFF) {
        message(why.isEmpty() ? QStringLiteral("The value is a byte (00-FF)") : why);
        return;
    }
    const uint8_t byte = uint8_t(val);
    msdosdebug_write(m_dbg, addr, &byte, 1);
    message(QStringLiteral("Wrote %1 to %2").arg(hexText(quint64(val), 2), hexText(addr, 5)));
    resync();
}

/* Find Next / Previous: from the highlighted bytes, or the top of the dump. */
void DebuggerWindow::memSearch(bool backward)
{
    uint8_t pat[MSDOSSEARCH_MAX], mask;
    int count = 0, flags = 0;
    uint32_t found;
    if (!msdossearch_parse(m_srchKind->currentIndex(), m_srchEntry->text().toUtf8().constData(), pat, &count, &mask, &flags)) {
        m_srchNote->setText(QStringLiteral("Type hex bytes (B8 34 12) or some text."));
        return;
    }
    if (!msdosdebug_stopped(m_dbg)) {
        m_srchNote->setText(QStringLiteral("Stop the PC to search its memory."));
        return;
    }
    const uint32_t from = m_memHl ? (backward ? m_memHlLo : m_memHlLo + 1) : m_memTop;
    const int hit = msdosdebug_search_next(m_dbg, pat, count, mask, flags, from, backward ? 1 : 0, &found);
    if (hit > 0) {
        m_srchNote->setText(QStringLiteral("Found at %1").arg(hexText(found, 5)));
        showInMemory(found, found + uint32_t(count) - 1);
    } else {
        m_srchNote->setText(hit == 0 ? QStringLiteral("Not found.") : QStringLiteral("Stop the PC to search its memory."));
    }
}

/* The Disassembly from a linear address (a symbol, a search hit). */
void DebuggerWindow::showInDisasm(uint32_t linear)
{
    /* in the code segment's own 64K: its seg:off; else the 64K page the address is in */
    uint16_t seg = 0;
    uint32_t off = linear;
    msdos_cpu_regs regs;
    if (msdosdebug_regs(m_dbg, &regs) == 0 && linear >= regs.cs_base && linear - regs.cs_base < 0x10000) {
        seg = regs.cs;
        off = linear - regs.cs_base;
    } else if (linear < 0x100000) {
        seg = uint16_t((linear >> 4) & 0xF000);
        off = linear - (uint32_t(seg) << 4);
    }
    m_followPc->setChecked(false);
    m_topSeg = seg;
    m_topOff = off;
    m_selected = linear;
    refreshDisasm();
}

void DebuggerWindow::jumpTo()
{
    uint16_t seg;
    uint32_t off, lin;
    if (!parseAddr(m_jump->text(), &seg, &off, &lin)) {
        message(QStringLiteral("Not an address (seg:off, cs:ip, a linear address, a symbol)"));
        return;
    }
    m_followPc->setChecked(false);
    m_topSeg = seg;
    m_topOff = off;
    m_selected = lin;
    refreshDisasm();
}

/* Execute and memory breakpoints take addresses; port and INT ones numbers.
 * The end is optional; AH only means something for an INT; the condition is an
 * expression the engine checks. */
void DebuggerWindow::addBreakpoint()
{
    int type = m_bpType->currentIndex();
    if (type < 0 || type >= MSDOS_BP_TYPES) type = MSDOS_BP_EXEC;
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed(), ahText = m_bpAh->text().trimmed();
    if (start.isEmpty()) return;
    uint32_t first, last;
    int ahValue = -1;
    if (type <= MSDOS_BP_ACCESS) {
        if (!addressOf(start, &first)) {
            m_bpMsg->setText(QStringLiteral("Not an address (seg:off, a linear address, a symbol)"));
            return;
        }
        last = first;
        if (!end.isEmpty() && !addressOf(end, &last)) {
            m_bpMsg->setText(QStringLiteral("The end is not an address"));
            return;
        }
    } else {
        int64_t num, upto;
        const int64_t limit = type == MSDOS_BP_INT ? 0xFF : 0xFFFF;
        if (!numberOf(start, &num, nullptr) || num < 0 || num > limit) {
            m_bpMsg->setText(type == MSDOS_BP_INT ? QStringLiteral("An interrupt is 00-FF")
                                                  : QStringLiteral("A port is 0000-FFFF"));
            return;
        }
        upto = num;
        if (!end.isEmpty() && type != MSDOS_BP_INT && (!numberOf(end, &upto, nullptr) || upto < num || upto > 0xFFFF)) {
            m_bpMsg->setText(QStringLiteral("The last port is a port at or after the first"));
            return;
        }
        first = uint32_t(num);
        last = uint32_t(upto);
        int64_t ahv;
        if (type == MSDOS_BP_INT && !ahText.isEmpty()) {
            if (!numberOf(ahText, &ahv, nullptr) || ahv < 0 || ahv > 0xFF) {
                m_bpMsg->setText(QStringLiteral("AH is 00-FF, or empty for any"));
                return;
            }
            ahValue = int(ahv);
        }
    }
    if (last < first) {
        m_bpMsg->setText(QStringLiteral("The end is before the start"));
        return;
    }
    char err[256] = "";
    const QByteArray cond = m_bpCond->text().trimmed().toUtf8();
    if (msdosdebug_bp_add_if(m_dbg, type, first, last, ahValue, cond.constData(), err, sizeof err) < 0) {
        m_bpMsg->setText(err[0] ? cleanText(err) : QStringLiteral("The breakpoint could not be added"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpAh->clear();
    m_bpCond->clear();
    resync();
}

void DebuggerWindow::bpTypeChanged(int type)
{
    m_bpAh->setEnabled(type == MSDOS_BP_INT);
    m_bpEnd->setEnabled(type != MSDOS_BP_INT);
    m_bpStart->setPlaceholderText(type <= MSDOS_BP_ACCESS ? QStringLiteral("1234:0010 / symbol")
                                  : type == MSDOS_BP_INT ? QStringLiteral("21") : QStringLiteral("2F8"));
    m_bpEnd->setPlaceholderText(type <= MSDOS_BP_ACCESS ? QStringLiteral("(end)")
                                : type == MSDOS_BP_INT ? QString() : QStringLiteral("(last port)"));
}

void DebuggerWindow::addWatch()
{
    if (msdoswatch_add(m_wtEntry->text().toUtf8().constData()) < 0) {
        message(QStringLiteral("Type an address, symbol, expression or first-last (the list holds %1)").arg(MSDOSWATCH_MAX));
        return;
    }
    m_wtEntry->clear();
    resync();
}

void DebuggerWindow::breakAtSymbol()
{
    QListWidgetItem *item = m_symList->currentItem();
    if (!item) {
        message(QStringLiteral("Select a symbol first"));
        return;
    }
    const uint32_t addr = item->data(Qt::UserRole).toUInt();
    const int now = msdosdebug_bp_toggle(m_dbg, addr);
    message(now ? QStringLiteral("Breakpoint at %1").arg(hexText(addr, 5)) : QStringLiteral("Breakpoint at %1 removed").arg(hexText(addr, 5)));
    resync();
}

void DebuggerWindow::loadSymbols()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load Symbols (MS LINK or Watcom WLINK .map)"),
                                                      QString(), QStringLiteral("Linker maps (*.map *.MAP);;All files (*)"));
    if (path.isEmpty()) return;
    int64_t seg = 0;
    if (!numberOf(m_symSeg->text(), &seg, nullptr) || seg < 0 || seg > 0xFFFF) seg = 0;
    char msg[512] = "";
    if (msdosdebug_load_symbols(m_dbg, path.toLocal8Bit().constData(), uint16_t(seg), msg, sizeof msg) < 0 && !msg[0])
        qstrncpy(msg, "No symbols found", sizeof msg);
    say(QString::fromUtf8(msg));
    refreshSymbols(true);
    resync();
}

void DebuggerWindow::save(const char *kind, const QString &title, const QString &file)
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save %1").arg(title), file);
    if (path.isEmpty()) return;
    char msg[512] = "";
    msdosdebug_save(m_dbg, kind, path.toLocal8Bit().constData(), msg, sizeof msg);
    say(msg[0] ? cleanText(msg) : QStringLiteral("Saved %1").arg(path));
}

/* ---- tabs peeled off into windows of their own --------------------------------- */

/* A page's place in the tab widget is not its identity once tabs can be
 * moved into windows of their own: each carries its original index. */
int DebuggerWindow::currentTab() const
{
    QWidget *page = m_tabs->currentWidget();
    return page ? page->property("tab").toInt() : -1;
}

/* On screen: the tab widget's page, or a window of its own. */
bool DebuggerWindow::tabVisible(int tab) const
{
    return currentTab() == tab || m_popped[tab] != nullptr;
}

/* Brings a tab forward, wherever it lives. */
void DebuggerWindow::showTab(int tab)
{
    if (m_popped[tab]) {
        m_popped[tab]->activateWindow();
        m_popped[tab]->raise();
    } else {
        m_tabs->setCurrentWidget(m_pages[tab]);
    }
}

void DebuggerWindow::popOut(int tab)
{
    if (tab < 0 || tab >= TAB_COUNT || m_popped[tab] || m_tabs->count() <= 1) return;   /* one tab always stays */
    QWidget *page = m_pages[tab];
    m_tabs->removeTab(m_tabs->indexOf(page));

    auto *win = new PopWindow;
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("Debugger – %1").arg(QString::fromUtf8(kTabs[tab].label)));
    win->setProperty("msdosHelpContext", QStringLiteral("debugger/%1").arg(QLatin1String(kTabs[tab].name)));
    auto *layout = new QVBoxLayout(win);
    auto *bar = new QHBoxLayout;
    auto *dockBtn = new QPushButton(QStringLiteral("Dock"));
    dockBtn->setToolTip(QStringLiteral("Put this tab back in the debugger"));
    bar->addWidget(dockBtn);
    bar->addStretch();
    layout->addLayout(bar);
    layout->addWidget(page, 1);
    page->show();
    win->resize(760, 520);
    m_popped[tab] = win;
    win->onClose = [this, tab] {
        if (!m_closing) dock(tab);
    };
    win->onKey = [this](QKeyEvent *evt) { return handleKey(evt); };
    connect(dockBtn, &QPushButton::clicked, win, &QWidget::close);
    new QShortcut(QKeySequence(Qt::Key_F1), win, [win] { msdosHelpFor(win); });
    win->show();
    if (msdossession_is_running(m_session)) resync();
}

void DebuggerWindow::dock(int tab)
{
    QWidget *win = m_popped[tab];
    if (!win) return;
    QWidget *page = m_pages[tab];
    m_popped[tab] = nullptr;
    int pos = 0;
    for (int before = 0; before < tab; before++)   /* back where it was in the order */
        if (!m_popped[before]) pos++;
    page->setParent(nullptr);
    m_tabs->insertTab(pos, page, tabLabel(tab));
    m_tabs->setCurrentIndex(pos);
    if (msdossession_is_running(m_session)) resync();
}

/* ---- events --------------------------------------------------------------- */

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *evt)
{
    /* Tab completes; Up and Down walk the history */
    if (obj == m_conIn && evt->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(evt);
        if (keyEvent->key() == Qt::Key_Tab) {
            completeConsole();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Up) {
            if (m_historyPos > 0) {
                --m_historyPos;
                showHistory();
            }
            return true;
        }
        if (keyEvent->key() == Qt::Key_Down) {
            if (m_historyPos < int(m_history.size())) {
                ++m_historyPos;
                showHistory();
            }
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. */
        if (evt->type() == QEvent::MouseButtonPress || evt->type() == QEvent::MouseButtonDblClick) {
            auto *mouseEvent = static_cast<QMouseEvent *>(evt);
            if (mouseEvent->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(mouseEvent->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= int(m_lineLin.size())) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = evt->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineLin[size_t(line)];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineLin[size_t(line)]);
            else refreshDisasm();
            return true;
        }
        if (evt->type() == QEvent::MouseButtonRelease || evt->type() == QEvent::MouseMove) return true;
        if (evt->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(evt)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0 && msdossession_is_running(m_session)) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_topOff = rows > 0 ? rowsOn(m_topSeg, m_topOff, rows) : rowsBack(m_topSeg, m_topOff, -rows);
                refreshDisasm();
            }
            return true;
        }
        if (evt->type() == QEvent::Resize && isVisible() && msdossession_is_running(m_session)) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, evt);
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *evt)
{
    if (evt->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(evt))) {
        evt->accept();
        return true;
    }
    return QMainWindow::event(evt);
}

/* The family's keys, from this window or one of its peeled-off tabs. */
bool DebuggerWindow::handleKey(QKeyEvent *evt)
{
    if (!isDebuggerKey(evt)) return false;
    const bool shift = evt->modifiers() & Qt::ShiftModifier;
    /* held, the steps repeat; the others do not */
    if (evt->isAutoRepeat() && (evt->key() == Qt::Key_F5 || evt->key() == Qt::Key_F9)) return true;
    switch (evt->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(msdosdebug_step); break;
    case Qt::Key_F8: stepAnd(shift ? msdosdebug_step_out : msdosdebug_step_over); break;
    default: toggleSelected(); break;     /* F9 */
    }
    return true;
}

void DebuggerWindow::keyPressEvent(QKeyEvent *evt)
{
    if (!handleKey(evt)) QMainWindow::keyPressEvent(evt);
}

/* A show that is not a restore from the taskbar brings the peeled-off tabs
 * back with it. */
void DebuggerWindow::showEvent(QShowEvent *evt)
{
    QMainWindow::showEvent(evt);
    if (evt->spontaneous()) return;
    for (QWidget *win : m_popped)
        if (win) win->show();
}

/* The scripted-capture variables that touch the machine (read at construction),
 * applied once the window is up: MSDOS_DEBUGGER_WATCH adds a watch,
 * MSDOS_DEBUGGER_WATCH_BREAK a write-when-changed breakpoint on it,
 * MSDOS_DEBUGGER_MEM_GOTO shows it in Memory, MSDOS_DEBUGGER_HISTORY records
 * from the start. (MSDOS_DEBUGGER_TAB and MSDOS_DEBUGGER_POPOUT are read at construction.) */
void DebuggerWindow::firstShow()
{
    if (m_shownOnce) return;
    m_shownOnce = true;
    uint32_t first, last;
    if (!m_envWatch.isEmpty()) msdoswatch_add(m_envWatch.constData());
    if (!m_envWatchBreak.isEmpty() && msdossym_parse_range(m_dbg, m_envWatchBreak.constData(), &first, &last)) {
        char err[200] = "";
        msdosdebug_bp_add_if(m_dbg, MSDOS_BP_WRITE, first, last, -1, "value != old", err, sizeof err);
    }
    if (!m_envMemGoto.isEmpty() && msdossym_parse_range(m_dbg, m_envMemGoto.constData(), &first, &last))
        showInMemory(first, last);
    if (m_envHistory) msdosdebug_history_enable(m_dbg, 1);   /* record from the start */
    resync();
}

/* Hidden by Ctrl+Shift+D, the close button or the main window: the engine
 * goes away and the machine runs on. A minimised window's spontaneous hide
 * is not a detach. The peeled-off tabs go with it. */
void DebuggerWindow::hideEvent(QHideEvent *evt)
{
    if (!evt->spontaneous()) {
        for (QWidget *win : m_popped)
            if (win) win->hide();
        if (!m_closing) {
            msdosdebug_detach(m_dbg);
            m_attachedRunning = false;
        }
    }
    QMainWindow::hideEvent(evt);
}

} // namespace

void msdosDebuggerShow(QWidget *parent, msdossession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
    s_win->attachAndRefresh();
    QTimer::singleShot(0, s_win.data(), [] {
        if (s_win && s_win->isVisible()) s_win->firstShow();
    });
}

void msdosDebuggerToggle(QWidget *parent, msdossession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else msdosDebuggerShow(parent, session);
}
