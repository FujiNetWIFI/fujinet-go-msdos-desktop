/*
 * Debugger window (Qt6 Widgets) over the session's debugger engine, via
 * core/include/msdosdebug.h. Mirrors the GNOME one tab for tab.
 *
 * Tabs: the Console (DEBUG.COM's commands a line at a time, with a command
 * history), CPU & Memory (registers, flags, a hex dump), the Disassembly
 * (click a line to select it for Run to Cursor; click its left edge, or
 * double-click it, for its breakpoint), Breakpoints (code, memory, I/O
 * ports, software interrupts), Interrupts (a DOS/BIOS call trace and the
 * vector table) and FujiNet (the FujiBus packets on its COM port and its
 * UART). The toolbar carries the stepping controls and symbol loading; the
 * family's keys apply (F5 run/stop, F7 step, F8 step over, Shift+F8 step
 * out).
 *
 * Showing the window attaches (which stops the machine, as on every
 * sibling); hiding it detaches and lets the machine run on.
 *
 * A short timer polls the engine's generation and refreshes the tab showing
 * when it moved; while the machine runs, the status, the registers and the
 * traces refresh twice a second.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DebuggerWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBlock>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "../FujiNetWindows.h"
#include "../help/HelpWindow.h"
#include "msdosdebug.h"

#define DISASM_WINDOW 64        /* lines fetched; as many show as fit */
#define MEM_ROWS 256            /* 4K of memory, 16 bytes a row */
#define BP_MAX 64
#define TRACE_MAX 512
#define CONSOLE_LINES 5000

namespace {

enum { TAB_CONSOLE, TAB_CPU, TAB_DISASM, TAB_BREAKS, TAB_INTS, TAB_FUJINET, TAB_COUNT };

/* The tabs, in order: the names MSDOS_DEBUGGER_TAB takes, and their labels
 * (which it takes too). */
const struct { const char *name, *label; } kTabs[TAB_COUNT] = {
    { "console", "Console" }, { "cpu", "CPU & Memory" }, { "disassembly", "Disassembly" },
    { "breakpoints", "Breakpoints" }, { "interrupts", "Interrupts" }, { "fujinet", "FujiNet" },
};

/* the general registers, as msdosdebug_set_reg names them */
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

void setMono(QWidget *w)
{
    QFont f = w->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    w->setFont(f);
}

QPlainTextEdit *monoView()
{
    auto *v = new QPlainTextEdit;
    v->setReadOnly(true);
    setMono(v);
    v->setLineWrapMode(QPlainTextEdit::NoWrap);
    return v;
}

/* Engine text for a view: control bytes dropped. */
QString cleanText(const QString &in)
{
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
        if (c.unicode() >= 0x20 || c == QLatin1Char('\n') || c == QLatin1Char('\t')) out += c;
    return out;
}
QString cleanText(const char *s) { return cleanText(QString::fromUtf8(s)); }

/* Hex, as DEBUG.COM takes it ("2F8", "0x2F8", "2F8h"); # for decimal. */
bool parseNum(const QString &text, long *out)
{
    QString s = text.trimmed();
    bool ok = false;
    long v;
    if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        v = s.mid(2).toLong(&ok, 16);
    } else if (s.startsWith(QLatin1Char('#'))) {
        v = s.mid(1).toLong(&ok, 10);
    } else {
        if (s.endsWith(QLatin1Char('h'), Qt::CaseInsensitive)) s.chop(1);
        v = s.toLong(&ok, 16);
    }
    if (ok) *out = v;
    return ok;
}

Q_ATTRIBUTE_FORMAT_PRINTF(2, 3)
void appendf(QString &t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    t += QString::vasprintf(fmt, ap);
    va_end(ap);
}

QLabel *titleLabel(const QString &text)
{
    auto *l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    return l;
}

QLabel *hintLabel(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(kGray);
    l->setWordWrap(true);
    return l;
}

QLineEdit *smallEntry(const QString &placeholder, int chars)
{
    auto *e = new QLineEdit;
    e->setPlaceholderText(placeholder);
    e->setFixedWidth(e->fontMetrics().averageCharWidth() * chars + 16);
    return e;
}

/* A register entry exactly wide enough for its hex digits. */
void setDigits(QLineEdit *e, int digits)
{
    e->setFixedWidth(e->fontMetrics().horizontalAdvance(QString(digits, QLatin1Char('0'))) + 16);
}

/* Whether a view's reader is at its end (a log then follows new lines). */
bool atEnd(QPlainTextEdit *v)
{
    return v->verticalScrollBar()->value() >= v->verticalScrollBar()->maximum() - 1;
}

/* Replaces a view's text where the reader left it (a refresh must not throw
 * a scrolled view back to its top), or, following, at its end. */
void setText(QPlainTextEdit *v, const QString &text, bool follow = false)
{
    if (v->toPlainText() == text) {
        if (follow) v->verticalScrollBar()->setValue(v->verticalScrollBar()->maximum());
        return;
    }
    const int top = v->verticalScrollBar()->value(), left = v->horizontalScrollBar()->value();
    v->setPlainText(text);
    v->verticalScrollBar()->setValue(follow ? v->verticalScrollBar()->maximum() : top);
    v->horizontalScrollBar()->setValue(left);
}

/* A whole line of a view in a format: the accent fill, a mark. */
QTextEdit::ExtraSelection lineSelection(QPlainTextEdit *v, int line, const QTextCharFormat &fmt)
{
    QTextEdit::ExtraSelection s;
    s.cursor = QTextCursor(v->document()->findBlockByNumber(line));
    s.format = fmt;
    s.format.setProperty(QTextFormat::FullWidthSelection, true);
    return s;
}

/* The accent fill (the IBM badge's silver) with the badge's ink on it: the
 * CS:IP line, a marked row. */
QTextCharFormat accentFormat()
{
    QTextCharFormat f;
    f.setBackground(msdosAccentColor());
    f.setForeground(msdosAccentInk());
    return f;
}

/* A status line that elides instead of widening the window. */
class ElidedLabel : public QLabel {
public:
    ElidedLabel() { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = contentsRect();
        p.setPen(palette().color(foregroundRole()));
        p.drawText(r, (alignment() & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter,
                   fontMetrics().elidedText(text(), Qt::ElideRight, r.width()));
    }
};

class DebuggerWindow : public QMainWindow {
public:
    DebuggerWindow(msdossession *session, QWidget *parent);
    ~DebuggerWindow() override;
    /* Attaches (stopping the machine) and refreshes. */
    void attachAndRefresh();

protected:
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *e) override;

private:
    QWidget *buildToolbar();
    QPushButton *buildSymbols();
    QWidget *buildConsole();
    QWidget *buildCpu();
    QWidget *buildDisasm();
    QWidget *buildBreaks();
    QWidget *buildInts();
    QWidget *buildFujinet();

    void refreshStatus();
    void refreshCpu();
    void refreshMem();
    void refreshDisasm();
    void refreshBps();
    void refreshInts(bool force);
    void refreshFujinet(bool force);
    void refreshPage(int page);
    void refreshAll();
    void resync();
    void syncLater();
    void settleAndSync();
    void tick();

    void consoleAppend(const QString &text);
    void consoleOutput(const QString &text);
    void consoleStop();
    void runConsole();
    void showHistory();
    void say(const QString &msg);
    void message(const QString &msg);

    void toggleRun();
    void stepAnd(int (*fn)(msdosdebug *));
    void runToCursor();
    void toggleBreakpoint(uint32_t linear);
    void addBreakpoint();
    void bpTypeChanged(int type);
    void setReg(int i, bool segment);
    void memGoto();
    void memWrite();
    void jumpTo();
    int visibleRows() const;
    uint32_t rowsBack(uint16_t seg, uint32_t off, int rows);
    uint32_t rowsOn(uint16_t seg, uint32_t off, int rows);
    void loadSymbols();
    bool parseAddr(const QString &text, uint16_t *seg, uint32_t *off, uint32_t *lin);
    void setEntry(QLineEdit *e, const QString &text);

    msdossession *m_session;
    msdosdebug *m_dbg;
    unsigned m_seenGen = 0;
    bool m_wasStopped = false;
    bool m_attachedRunning = false;     /* attached while the machine was running */
    int m_runningTicks = 0;
    QTimer m_timer;

    QTabWidget *m_tabs = nullptr;
    ElidedLabel *m_status = nullptr;
    QPushButton *m_runBtn = nullptr;

    /* console */
    QPlainTextEdit *m_conOut = nullptr;
    QLineEdit *m_conIn = nullptr;
    QStringList m_history;
    int m_historyPos = 0;

    /* cpu + memory */
    QLabel *m_regLabel[NREGS] = {};
    QLineEdit *m_reg[NREGS] = {};
    QWidget *m_sregCell[NSREGS] = {};
    QLineEdit *m_sreg[NSREGS] = {};
    QLabel *m_sbase[NSREGS] = {};
    QCheckBox *m_flag[NFLAGS] = {};
    QLabel *m_cpuInfo = nullptr;
    QPlainTextEdit *m_mem = nullptr;
    QLineEdit *m_memGoto = nullptr, *m_memAddr = nullptr, *m_memVal = nullptr;
    uint32_t m_memTop = 0;              /* linear */
    int64_t m_memMark = -1;             /* the address Go to found; -1 none */
    bool m_is386 = false;

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
    QLabel *m_bpEmpty = nullptr, *m_bpMsg = nullptr;
    QComboBox *m_bpType = nullptr;
    QLineEdit *m_bpStart = nullptr, *m_bpEnd = nullptr, *m_bpAh = nullptr;

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

    /* symbols */
    QLineEdit *m_symSeg = nullptr;
};

QPointer<DebuggerWindow> s_win;

/* MSDOS_DEBUGGER_TAB: a page number, or a tab's name or label (any case), or
 * the start of one, or of a word of a label ("memory" ...). */
int tabFromEnv()
{
    const QString tab = qEnvironmentVariable("MSDOS_DEBUGGER_TAB").trimmed();
    if (tab.isEmpty()) return -1;
    bool isNum = false;
    const int n = tab.toInt(&isNum);
    if (isNum) return n >= 0 && n < TAB_COUNT ? n : -1;
    for (int pass = 0; pass < 3; ++pass)
        for (int i = 0; i < TAB_COUNT; ++i) {
            const QString name = QString::fromUtf8(kTabs[i].name), label = QString::fromUtf8(kTabs[i].label);
            if (pass == 0 && name.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 1 && label.startsWith(tab, Qt::CaseInsensitive)) return i;
            if (pass == 2) {
                const QStringList words = label.split(QLatin1Char(' '));
                for (int w = 1; w < words.size(); ++w)
                    if (words[w].startsWith(tab, Qt::CaseInsensitive)) return i;
            }
        }
    return -1;
}

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
    pages[TAB_CPU] = buildCpu();
    pages[TAB_DISASM] = buildDisasm();
    pages[TAB_BREAKS] = buildBreaks();
    pages[TAB_INTS] = buildInts();
    pages[TAB_FUJINET] = buildFujinet();
    for (int i = 0; i < TAB_COUNT; ++i)
        m_tabs->addTab(pages[i], QString::fromUtf8(kTabs[i].label).replace(QLatin1Char('&'), QLatin1String("&&")));
    const int tab = tabFromEnv();
    if (tab >= 0) m_tabs->setCurrentIndex(tab);
    /* the Help for the tab: Ctrl+Shift+H, F1 and the Help button */
    auto helpContext = [this](int page) {
        setProperty("msdosHelpContext", QStringLiteral("debugger/") + QLatin1String(kTabs[page].name));
    };
    helpContext(m_tabs->currentIndex());
    connect(m_tabs, &QTabWidget::currentChanged, this, [this, helpContext](int page) {
        helpContext(page);
        refreshPage(page);
    });
    root->addWidget(m_tabs, 1);
    setCentralWidget(central);
    statusBar();

    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_timer.start(100);
}

/* Gone with the main window (the session outlives it): the machine runs on. */
DebuggerWindow::~DebuggerWindow()
{
    m_timer.stop();
    msdosdebug_detach(m_dbg);
}

/* ---- building ------------------------------------------------------------- */

QWidget *DebuggerWindow::buildToolbar()
{
    auto *bar = new QWidget;
    auto *h = new QHBoxLayout(bar);
    h->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const QString &label, const QString &tip, std::function<void()> fn) {
        auto *b = new QPushButton(label);
        b->setFocusPolicy(Qt::NoFocus);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        h->addWidget(b);
        return b;
    };
    m_runBtn = add(QStringLiteral("Stop (F5)"), QStringLiteral("Run, or stop before the next instruction"),
                   [this] { toggleRun(); });
    add(QStringLiteral("Step (F7)"), QStringLiteral("One instruction"), [this] { stepAnd(msdosdebug_step); });
    add(QStringLiteral("Step Over (F8)"), QStringLiteral("Over a CALL, INT, LOOP or REP, to the instruction after it"),
        [this] { stepAnd(msdosdebug_step_over); });
    add(QStringLiteral("Step Out (⇧F8)"), QStringLiteral("On until the routine returns (RET, RETF or IRET)"),
        [this] { stepAnd(msdosdebug_step_out); });
    add(QStringLiteral("Run to Cursor"), QStringLiteral("On to the line selected in the Disassembly"),
        [this] { runToCursor(); });
    m_status = new ElidedLabel;
    m_status->setAlignment(Qt::AlignRight);
    m_status->setStyleSheet(kGray);
    m_status->setContentsMargins(8, 0, 8, 0);
    h->addWidget(m_status, 1);
    h->addWidget(buildSymbols());
    h->addWidget(msdosHelpButton(QString()));
    return bar;
}

/* Symbols, from a linker map at a load segment. */
QPushButton *DebuggerWindow::buildSymbols()
{
    auto *btn = new QPushButton(QStringLiteral("Symbols"));
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setToolTip(QStringLiteral("Labels from an MS LINK or Watcom WLINK .map file"));
    auto *menu = new QMenu(btn);
    auto *panel = new QWidget;
    panel->setFixedWidth(300);
    auto *v = new QVBoxLayout(panel);
    v->setContentsMargins(10, 10, 10, 10);
    v->addWidget(titleLabel(QStringLiteral("Symbols")));
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("Load segment")));
    m_symSeg = smallEntry(QStringLiteral("0000"), 6);
    row->addWidget(m_symSeg);
    row->addStretch(1);
    v->addLayout(row);
    v->addWidget(hintLabel(QStringLiteral("The segment the program was loaded at: a .COM's PSP, an .EXE's PSP + 10h.")));
    auto *load = new QPushButton(QStringLiteral("Choose a .map…"));
    connect(load, &QPushButton::clicked, this, [this, menu] {
        menu->close();
        loadSymbols();
    });
    auto *clear = new QPushButton(QStringLiteral("Clear Symbols"));
    clear->setFlat(true);
    connect(clear, &QPushButton::clicked, this, [this, menu] {
        menu->close();
        msdosdebug_clear_symbols(m_dbg);
        say(QStringLiteral("Symbols cleared"));
        resync();
    });
    v->addWidget(load);
    v->addWidget(clear);
    auto *act = new QWidgetAction(menu);
    act->setDefaultWidget(panel);
    menu->addAction(act);
    btn->setMenu(menu);
    return btn;
}

QWidget *DebuggerWindow::buildConsole()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
    m_conOut = monoView();
    m_conOut->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_conOut->setMaximumBlockCount(CONSOLE_LINES);
    m_conOut->setPlainText(QStringLiteral("DEBUG.COM's commands: r d e u g t p, plus bp/bpr/bpw/bpi/bpo/bpint, bl, "
                                          "bc, i, o. Type h for the list; Up/Down recall.\n"));
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
    v->addWidget(m_conOut, 1);
    v->addLayout(row);
    return w;
}

QWidget *DebuggerWindow::buildCpu()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    /* the general registers across, the segment registers (with their
     * bases) below */
    auto *regs = new QGridLayout;
    regs->setHorizontalSpacing(8);
    regs->setVerticalSpacing(4);
    auto regEntry = [](int chars) {
        auto *e = new QLineEdit;
        setMono(e);
        e->setMaxLength(10);
        setDigits(e, chars);
        e->setToolTip(QStringLiteral("Enter sets it (hex)"));
        return e;
    };
    for (int i = 0; i < NREGS; ++i) {
        auto *cell = new QWidget;
        auto *h = new QHBoxLayout(cell);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        m_regLabel[i] = new QLabel(QString::fromLatin1(kRegs16[i]).toUpper());
        m_regLabel[i]->setStyleSheet(kGray);
        m_regLabel[i]->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_regLabel[i]->setFixedWidth(m_regLabel[i]->fontMetrics().horizontalAdvance(QStringLiteral("EAX")));
        m_reg[i] = regEntry(4);
        connect(m_reg[i], &QLineEdit::returnPressed, this, [this, i] { setReg(i, false); });
        h->addWidget(m_regLabel[i]);
        h->addWidget(m_reg[i]);
        regs->addWidget(cell, 0, i);
    }
    for (int i = 0; i < NSREGS; ++i) {
        auto *cell = new QWidget;
        auto *h = new QHBoxLayout(cell);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        auto *l = new QLabel(QString::fromLatin1(kSregs[i]).toUpper());
        l->setStyleSheet(kGray);
        m_sreg[i] = regEntry(4);
        connect(m_sreg[i], &QLineEdit::returnPressed, this, [this, i] { setReg(i, true); });
        h->addWidget(l);
        h->addWidget(m_sreg[i]);
        h->addStretch(1);
        m_sregCell[i] = cell;
        regs->addWidget(cell, 1, i);
        m_sbase[i] = new QLabel;
        m_sbase[i]->setStyleSheet(kGray);
        QFont small = m_sbase[i]->font();
        small.setPointSizeF((small.pointSizeF() > 0 ? small.pointSizeF() : 10) * 0.85);
        m_sbase[i]->setFont(small);
        regs->addWidget(m_sbase[i], 2, i);
    }
    regs->setColumnStretch(NREGS, 1);
    v->addLayout(regs);

    auto *flags = new QHBoxLayout;
    for (int i = 0; i < NFLAGS; ++i) {
        m_flag[i] = new QCheckBox(QString::fromLatin1(kFlags[i].name).toUpper());
        connect(m_flag[i], &QCheckBox::clicked, this, [this, i](bool on) {
            if (!msdosdebug_stopped(m_dbg)) {
                resync();
                return;
            }
            msdosdebug_set_reg(m_dbg, kFlags[i].name, on ? 1 : 0);
            resync();
        });
        flags->addWidget(m_flag[i]);
    }
    flags->addSpacing(16);
    m_cpuInfo = new ElidedLabel;
    m_cpuInfo->setStyleSheet(kGray);
    flags->addWidget(m_cpuInfo, 1);
    v->addLayout(flags);

    v->addWidget(titleLabel(QStringLiteral("Memory (linear; video RAM at A0000-BFFFF reads as the CPU would)")));
    m_mem = monoView();
    v->addWidget(m_mem, 1);

    /* the dump: Go to, a page back or on, and a write */
    auto *nav = new QHBoxLayout;
    nav->addWidget(new QLabel(QStringLiteral("Go to")));
    m_memGoto = smallEntry(QStringLiteral("ds:si / B800:0 / symbol"), 22);
    connect(m_memGoto, &QLineEdit::returnPressed, this, [this] { memGoto(); });
    nav->addWidget(m_memGoto);
    for (int dir = -1; dir <= 1; dir += 2) {
        auto *b = new QToolButton;
        b->setArrowType(dir < 0 ? Qt::UpArrow : Qt::DownArrow);
        b->setToolTip(dir < 0 ? QStringLiteral("Back 4K") : QStringLiteral("On 4K"));
        connect(b, &QToolButton::clicked, this, [this, dir] {
            if (dir < 0) m_memTop = m_memTop > MEM_ROWS * 16 ? m_memTop - MEM_ROWS * 16 : 0;
            else m_memTop += MEM_ROWS * 16;
            refreshMem();
        });
        nav->addWidget(b);
    }
    nav->addSpacing(12);
    nav->addWidget(new QLabel(QStringLiteral("Write")));
    m_memAddr = smallEntry(QStringLiteral("0040:0017"), 12);
    m_memVal = smallEntry(QStringLiteral("00"), 4);
    connect(m_memAddr, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    connect(m_memVal, &QLineEdit::returnPressed, this, [this] { memWrite(); });
    nav->addWidget(m_memAddr);
    nav->addWidget(new QLabel(QStringLiteral("value")));
    nav->addWidget(m_memVal);
    nav->addStretch(1);
    v->addLayout(nav);
    return w;
}

QWidget *DebuggerWindow::buildDisasm()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);
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
    row->addWidget(hintLabel(QStringLiteral("Click a line to select it (Run to Cursor); click its left edge or "
                                            "double-click it for a breakpoint (* on, o off); scroll to browse")),
                   1);
    v->addLayout(row);
    m_disasm = monoView();
    /* no scrollbar: the wheel scrolls by instructions, as many lines as fit */
    m_disasm->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_disasm->setFocusPolicy(Qt::NoFocus);
    m_disasm->viewport()->setCursor(Qt::ArrowCursor);
    m_disasm->viewport()->installEventFilter(this);
    v->addWidget(m_disasm, 1);
    return w;
}

QWidget *DebuggerWindow::buildBreaks()
{
    auto *w = new QWidget;
    auto *v = new QVBoxLayout(w);

    auto *top = new QHBoxLayout;
    auto *clear = new QPushButton(QStringLiteral("Clear All"));
    connect(clear, &QPushButton::clicked, this, [this] {
        msdosdebug_bp_clear(m_dbg);
        resync();
    });
    top->addWidget(hintLabel(QStringLiteral("Execute stops before the instruction runs; memory, ports and INT after "
                                            "the instruction that touched them.")), 1);
    top->addWidget(clear);
    v->addLayout(top);

    m_bpTable = new QTableWidget(0, 5);
    m_bpTable->setHorizontalHeaderLabels({ QStringLiteral("On"), QStringLiteral("#"), QStringLiteral("Breakpoint"),
                                           QStringLiteral("Hits"), QString() });
    m_bpTable->verticalHeader()->setVisible(false);
    m_bpTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_bpTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_bpTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_bpTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_bpTable->setFocusPolicy(Qt::NoFocus);
    connect(m_bpTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_bpFilling || item->column() != 0) return;
        msdosdebug_bp_enable(m_dbg, item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
        syncLater();
    });
    v->addWidget(m_bpTable, 1);
    m_bpEmpty = hintLabel(QStringLiteral("No breakpoints. Click the left edge of a disassembly line, or add one below."));
    v->addWidget(m_bpEmpty);

    auto *row = new QHBoxLayout;
    m_bpType = new QComboBox;
    m_bpType->addItems({ QStringLiteral("Execute"), QStringLiteral("Memory read"), QStringLiteral("Memory write"),
                         QStringLiteral("Memory read/write"), QStringLiteral("Port IN"), QStringLiteral("Port OUT"),
                         QStringLiteral("Port IN/OUT"), QStringLiteral("INT") });
    m_bpStart = smallEntry(QStringLiteral("1234:0010 / symbol"), 20);
    m_bpEnd = smallEntry(QStringLiteral("(end)"), 12);
    m_bpAh = smallEntry(QStringLiteral("AH (any)"), 8);
    m_bpAh->setEnabled(false);
    connect(m_bpType, &QComboBox::currentIndexChanged, this, [this](int t) { bpTypeChanged(t); });
    auto *add = new QPushButton(QStringLiteral("Add"));
    connect(add, &QPushButton::clicked, this, [this] { addBreakpoint(); });
    for (QLineEdit *e : { m_bpStart, m_bpEnd, m_bpAh })
        connect(e, &QLineEdit::returnPressed, this, [this] { addBreakpoint(); });
    row->addWidget(m_bpType);
    row->addWidget(m_bpStart);
    row->addWidget(m_bpEnd);
    row->addWidget(m_bpAh);
    row->addWidget(add);
    row->addStretch(1);
    v->addLayout(row);

    m_bpMsg = new QLabel;
    m_bpMsg->setWordWrap(true);
    m_bpMsg->setStyleSheet(QStringLiteral("color: #e01b24;"));
    v->addWidget(m_bpMsg);
    return w;
}

QWidget *DebuggerWindow::buildInts()
{
    auto *split = new QSplitter(Qt::Horizontal);
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    auto *row = new QHBoxLayout;
    row->addWidget(titleLabel(QStringLiteral("INT calls")), 1);
    m_intOn = new QCheckBox(QStringLiteral("Trace"));
    m_intOn->setToolTip(QStringLiteral("Record every INT the PC runs (costs speed)"));
    connect(m_intOn, &QCheckBox::clicked, this, [this](bool on) {
        msdosdebug_int_trace_enable(m_dbg, on);
        refreshInts(true);
    });
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        msdosdebug_int_trace_clear(m_dbg);
        refreshInts(true);
    });
    row->addWidget(m_intOn);
    row->addWidget(clear);
    lv->addLayout(row);
    m_intText = monoView();
    lv->addWidget(m_intText, 1);
    left->setMinimumWidth(520);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("Vector table")));
    m_ivtText = monoView();
    /* four vectors a row, as they are laid out */
    m_ivtText->setMinimumWidth(m_ivtText->fontMetrics().horizontalAdvance(
                                   QStringLiteral("00 0000:0000   01 0000:0000   02 0000:0000   03 0000:0000"))
                               + m_ivtText->verticalScrollBar()->sizeHint().width()
                               + 2 * (m_ivtText->frameWidth() + (int)m_ivtText->document()->documentMargin()) + 4);
    rv->addWidget(m_ivtText, 1);
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
    auto *lv = new QVBoxLayout(left);
    lv->addWidget(titleLabel(QStringLiteral("FujiBus packets (SLIP-decoded)")));
    auto *row = new QHBoxLayout;
    m_fujiNote = hintLabel(QString());
    auto *clear = new QPushButton(QStringLiteral("Clear"));
    connect(clear, &QPushButton::clicked, this, [this] {
        msdosdebug_fujinet_trace_clear(m_dbg);
        refreshFujinet(true);
    });
    row->addWidget(m_fujiNote, 1);
    row->addWidget(clear);
    lv->addLayout(row);
    m_fujiText = monoView();
    lv->addWidget(m_fujiText, 1);
    left->setMinimumWidth(560);

    auto *right = new QWidget;
    auto *rv = new QVBoxLayout(right);
    rv->addWidget(titleLabel(QStringLiteral("FujiNet's UART")));
    m_uartText = monoView();
    rv->addWidget(m_uartText, 1);
    right->setMinimumWidth(320);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setChildrenCollapsible(false);
    return split;
}

/* ---- refresh -------------------------------------------------------------- */

void DebuggerWindow::refreshStatus()
{
    char reason[160];
    const bool running = msdossession_is_running(m_session) != 0;
    const bool stopped = running && msdosdebug_stopped(m_dbg);
    QString text;
    if (!running) {
        text = QStringLiteral("The PC is not running");
    } else if (stopped) {
        msdosdebug_stop_reason(m_dbg, reason, sizeof reason);
        text = QStringLiteral("Stopped%1%2").arg(reason[0] ? QStringLiteral(": ") : QString(), QString::fromUtf8(reason));
    } else {
        text = QStringLiteral("Running");
    }
    if (m_status->text() != text) {
        m_status->setText(text);
        m_status->setToolTip(text);
        m_status->update();
    }
    m_runBtn->setText(stopped ? QStringLiteral("Run (F5)") : QStringLiteral("Stop (F5)"));
    m_runBtn->setStyleSheet(stopped ? msdosAccentStyle() : QString());
}

/* The live refresh does not overwrite a value being typed: while the machine
 * runs, a focused entry keeps what is in it. */
void DebuggerWindow::setEntry(QLineEdit *e, const QString &text)
{
    if (e->hasFocus() && !msdosdebug_stopped(m_dbg)) return;
    if (e->text() != text) e->setText(text);
}

void DebuggerWindow::refreshCpu()
{
    msdos_cpu_regs r;
    if (msdosdebug_regs(m_dbg, &r) != 0) return;
    const uint32_t vals[NREGS] = { r.eax, r.ebx, r.ecx, r.edx, r.esi, r.edi, r.ebp, r.esp, r.eip };
    const uint16_t svals[NSREGS] = { r.cs, r.ds, r.es, r.ss, r.fs, r.gs };
    const uint32_t bases[NSREGS] = { r.cs_base, r.ds_base, r.es_base, r.ss_base, r.fs_base, r.gs_base };
    if (m_is386 != (r.is386 != 0)) {
        /* the entries as wide as the registers: 8 digits only from the 386 */
        m_is386 = r.is386 != 0;
        for (QLineEdit *e : m_reg) setDigits(e, m_is386 ? 8 : 4);
    }
    for (int i = 0; i < NREGS; ++i) {
        m_regLabel[i]->setText(QString::fromLatin1(m_is386 ? kRegs32[i] : kRegs16[i]).toUpper());
        setEntry(m_reg[i], m_is386 ? QString::asprintf("%08X", vals[i]) : QString::asprintf("%04X", vals[i] & 0xFFFFu));
    }
    for (int i = 0; i < NSREGS; ++i) {
        const bool shown = i < 4 || m_is386;
        m_sregCell[i]->setVisible(shown);
        m_sbase[i]->setVisible(shown);
        setEntry(m_sreg[i], QString::asprintf("%04X", svals[i]));
        m_sbase[i]->setText(QString::asprintf("base %05X", bases[i]));
    }
    for (int i = 0; i < NFLAGS; ++i) m_flag[i]->setChecked((r.eflags >> kFlags[i].bit) & 1);
    static const char *const modes[] = { "real mode", "protected mode", "virtual 8086 mode" };
    char video[160] = "";
    msdosdebug_video_info(m_dbg, video, sizeof video);
    const QString vid = QString::fromUtf8(video).trimmed();
    QString info;
    if (r.is386)
        info = QStringLiteral("%1   %2   CR0 %3   %4").arg(QString::fromUtf8(r.cpu), QLatin1String(modes[qBound(0, r.mode, 2)]),
                                                         QString::asprintf("%08X", r.cr0), vid);
    else
        info = QStringLiteral("%1   %2").arg(QString::fromUtf8(r.cpu), vid);
    m_cpuInfo->setText(info);
    m_cpuInfo->setToolTip(info);
    m_cpuInfo->update();
}

void DebuggerWindow::refreshMem()
{
    static uint8_t mem[MEM_ROWS * 16];
    msdosdebug_read(m_dbg, m_memTop, mem, (int)sizeof mem);
    QString t;
    t.reserve(MEM_ROWS * 80);
    t += QStringLiteral("          ");
    for (int col = 0; col < 16; ++col) appendf(t, col == 8 ? " %X  " : "%X  ", col);
    for (int row = 0; row < MEM_ROWS; ++row) {
        const uint8_t *r = mem + row * 16;
        const uint32_t a = m_memTop + (uint32_t)row * 16;
        /* real-mode style, segment the address's own 64K */
        appendf(t, "\n%04X:%04X ", (a >> 4) & 0xF000, a & 0xFFFF);
        for (int col = 0; col < 16; ++col) appendf(t, col == 8 ? " %02X " : "%02X ", r[col]);
        t += QLatin1Char(' ');
        for (int col = 0; col < 16; ++col) t += QLatin1Char(r[col] >= 0x20 && r[col] < 0x7F ? (char)r[col] : '.');
    }
    setText(m_mem, t);

    /* the row Go to found stays marked across refreshes */
    QList<QTextEdit::ExtraSelection> sel;
    if (m_memMark >= 0 && (uint32_t)m_memMark >= m_memTop) {
        const uint32_t off = (uint32_t)m_memMark - m_memTop;
        if (off < MEM_ROWS * 16) sel << lineSelection(m_mem, 1 + (int)off / 16, accentFormat());
    }
    m_mem->setExtraSelections(sel);
}

/* The disassembly lines that fit the view. */
int DebuggerWindow::visibleRows() const
{
    const int line = m_disasm->fontMetrics().lineSpacing();
    const int height = m_disasm->viewport()->height() - 2 * (int)m_disasm->document()->documentMargin();
    if (line <= 0 || height <= 0) return 32;
    return qBound(4, height / line, DISASM_WINDOW);
}

/* x86 code cannot be read backwards: the offset rows instructions before off
 * is the nearest start whose instructions land on off. */
uint32_t DebuggerWindow::rowsBack(uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW * 4];
    const int count = qMin(rows * 7 + 1, (int)(sizeof lines / sizeof lines[0]));
    for (int k = rows; k <= rows * 7 && (uint32_t)k <= off; k++) {
        const int n = msdosdebug_disassemble(m_dbg, seg, off - (uint32_t)k, count, lines);
        int i = 0;
        while (i < n && lines[i].off < off) i++;
        if (i < n && lines[i].off == off && i >= rows) return lines[i - rows].off;
    }
    return off > (uint32_t)rows * 3 ? off - (uint32_t)rows * 3 : 0;
}

uint32_t DebuggerWindow::rowsOn(uint16_t seg, uint32_t off, int rows)
{
    static msdos_dis_line lines[DISASM_WINDOW + 1];
    const int n = msdosdebug_disassemble(m_dbg, seg, off, qMin(rows + 1, DISASM_WINDOW + 1), lines);
    return n > rows ? lines[rows].off : off;
}

void DebuggerWindow::refreshDisasm()
{
    static msdos_dis_line lines[DISASM_WINDOW];
    int bp[DISASM_WINDOW];
    const int rows = visibleRows();
    msdos_cpu_regs r;
    const bool haveRegs = msdosdebug_regs(m_dbg, &r) == 0;

    /* Following, CS:IP stays put while it shows, else sits a third of the
     * way down. */
    if (haveRegs && m_followPc->isChecked()) {
        const uint32_t pc = msdosdebug_linear(m_dbg, r.cs, r.eip);
        bool shows = false;
        for (int i = 1; i < (int)m_lineLin.size() && i < rows - 2; ++i)
            if (m_lineLin[(size_t)i] == pc) shows = true;
        if (!shows || m_topSeg != r.cs) {
            m_topSeg = r.cs;
            m_topOff = rowsBack(r.cs, r.eip, rows / 3);
        }
    }

    const int n = msdosdebug_disassemble(m_dbg, m_topSeg, m_topOff, qMin(rows + 1, DISASM_WINDOW), lines);
    int pcLine = -1;
    m_lineLin.clear();
    QString t;
    for (int i = 0; i < n; ++i) {
        const msdos_dis_line *l = &lines[i];
        const bool isPc = haveRegs && l->seg == r.cs && l->off == r.eip;
        bp[i] = msdosdebug_bp_at(m_dbg, l->linear);    /* 1 on, 2 off */
        m_lineLin.push_back(l->linear);
        if (isPc) pcLine = i;
        appendf(t, "%s%c%c %04X:%0*X  %-20s %s%s%s", i ? "\n" : "", bp[i] == 2 ? 'o' : bp[i] ? '*' : ' ',
                isPc ? '>' : ' ', l->seg, l->off > 0xFFFF ? 8 : 4, l->off, l->bytes, l->symbol,
                l->symbol[0] ? ": " : "", l->text);
    }
    if (n == 0) t = QStringLiteral("(no disassembly)");
    m_disasm->setPlainText(cleanText(t));
    m_disasm->verticalScrollBar()->setValue(0);

    QList<QTextEdit::ExtraSelection> sel;
    for (int i = 0; i < n; ++i) {
        /* the gutter: a breakpoint red, a disabled one grey */
        if (bp[i]) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
            if (bp[i] == 2) {
                s.format.setForeground(QColor(0x9a, 0x99, 0x96));
            } else {
                s.format.setForeground(QColor(0xe0, 0x1b, 0x24));
                s.format.setFontWeight(QFont::Bold);
            }
            sel << s;
        }
        /* the selected line underlined */
        if ((int64_t)lines[i].linear == m_selected && i != pcLine) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(m_disasm->document()->findBlockByNumber(i));
            s.cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            s.format.setFontUnderline(true);
            s.format.setFontWeight(QFont::Bold);
            sel << s;
        }
    }
    /* the CS:IP line in the accent colour, last so its ink wins */
    if (pcLine >= 0) {
        QTextCharFormat f = accentFormat();
        if ((int64_t)lines[pcLine].linear == m_selected) f.setFontUnderline(true);
        sel << lineSelection(m_disasm, pcLine, f);
    }
    m_disasm->setExtraSelections(sel);
}

void DebuggerWindow::refreshBps()
{
    static msdos_breakpoint bps[BP_MAX];
    const int n = msdosdebug_bp_list(m_dbg, bps, BP_MAX);

    /* rebuilt only when it changed (each step refreshes) */
    QString sig;
    for (int i = 0; i < n; ++i)
        appendf(sig, "%d %d %u %s\n", bps[i].id, bps[i].enabled, bps[i].hits, bps[i].text);
    if (sig == m_bpSig && m_bpTable->rowCount() == n) return;
    m_bpSig = sig;

    QFont mono = m_bpTable->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_bpFilling = true;
    m_bpTable->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        auto *on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        on->setCheckState(bps[i].enabled ? Qt::Checked : Qt::Unchecked);
        on->setData(Qt::UserRole, bps[i].id);
        on->setToolTip(QStringLiteral("Enabled"));
        m_bpTable->setItem(i, 0, on);
        m_bpTable->setItem(i, 1, new QTableWidgetItem(QStringLiteral("#%1").arg(bps[i].id)));
        m_bpTable->setItem(i, 2, new QTableWidgetItem(cleanText(bps[i].text)));
        m_bpTable->setItem(i, 3, new QTableWidgetItem(QString::number(bps[i].hits)));
        for (int col = 1; col < 4; ++col) m_bpTable->item(i, col)->setFont(mono);
        auto *rm = new QToolButton;
        rm->setIcon(QIcon::fromTheme(QStringLiteral("edit-delete"), QIcon::fromTheme(QStringLiteral("user-trash"))));
        if (rm->icon().isNull()) rm->setText(QStringLiteral("Remove"));
        rm->setAutoRaise(true);
        rm->setToolTip(QStringLiteral("Remove"));
        const int id = bps[i].id;
        connect(rm, &QToolButton::clicked, this, [this, id] {
            msdosdebug_bp_remove(m_dbg, id);
            syncLater();
        });
        m_bpTable->setCellWidget(i, 4, rm);
    }
    m_bpFilling = false;
    m_bpEmpty->setVisible(n == 0);
}

void DebuggerWindow::refreshInts(bool force)
{
    static msdos_int_event e[TRACE_MAX];
    const int n = msdosdebug_int_trace(m_dbg, e, TRACE_MAX);
    m_intOn->setChecked(msdosdebug_int_trace_enabled(m_dbg) != 0);

    /* the vector table: changes rarely, cheap to read */
    QString t;
    for (int i = 0; i < 256; ++i) {
        uint16_t seg = 0, off = 0;
        msdosdebug_ivt(m_dbg, i, &seg, &off);
        appendf(t, "%02X %04X:%04X%s", i, seg, off, (i & 3) == 3 ? (i < 255 ? "\n" : "") : "   ");
    }
    setText(m_ivtText, t);

    /* a log: set again only when it moved */
    if (!force && n == m_intCount && (n == 0 || e[n - 1].serial == m_intLast)) return;
    m_intCount = n;
    m_intLast = n > 0 ? e[n - 1].serial : 0;
    QString log = QStringLiteral("From       AX   BX   CX   DX   DS   ES    Call");
    for (int i = 0; i < n; ++i)
        appendf(log, "\n%04X:%04X  %04X %04X %04X %04X %04X %04X  %s", e[i].cs, e[i].ip & 0xFFFF, e[i].ax, e[i].bx,
                e[i].cx, e[i].dx, e[i].ds, e[i].es, e[i].text);
    if (n == 0)
        log += msdosdebug_int_trace_enabled(m_dbg) ? QStringLiteral("\n(no INT calls since the trace was cleared)")
                                                   : QStringLiteral("\n(turn the trace on to record INT calls; it costs speed)");
    setText(m_intText, cleanText(log), force || atEnd(m_intText));
}

void DebuggerWindow::refreshFujinet(bool force)
{
    static msdos_fuji_packet p[TRACE_MAX];
    static char uart[2048];
    const int n = msdosdebug_fujinet_trace(m_dbg, p, TRACE_MAX);
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

    if (!force && n == m_fujiCount && (n == 0 || p[n - 1].serial == m_fujiLast)) return;
    m_fujiCount = n;
    m_fujiLast = n > 0 ? p[n - 1].serial : 0;
    QString t;
    for (int i = 0; i < n; ++i) {
        appendf(t, "%s%s\n     ", i ? "\n" : "", p[i].text);
        for (int k = 0; k < p[i].nbytes && k < (int)sizeof p[i].data; k++) appendf(t, "%02X ", p[i].data[k]);
        if (p[i].nbytes > (int)sizeof p[i].data) t += QStringLiteral("…");
    }
    if (n == 0) t = QStringLiteral("(no FujiBus packets since the trace was cleared)");
    setText(m_fujiText, cleanText(t), force || atEnd(m_fujiText));
}

void DebuggerWindow::refreshPage(int page)
{
    if (!msdossession_is_running(m_session)) return;
    switch (page) {
    case TAB_CPU: refreshCpu(); refreshMem(); break;
    case TAB_DISASM: refreshDisasm(); break;
    case TAB_BREAKS: refreshBps(); break;
    case TAB_INTS: refreshInts(false); break;
    case TAB_FUJINET: refreshFujinet(false); break;
    default: break;
    }
}

void DebuggerWindow::refreshAll()
{
    refreshStatus();
    refreshPage(m_tabs->currentIndex());
}

/* After anything that may have moved the engine: the console takes the
 * registers at a new stop, then the views follow. */
void DebuggerWindow::resync()
{
    const bool stopped = msdosdebug_stopped(m_dbg) != 0;
    const unsigned gen = msdosdebug_generation(m_dbg);
    if (stopped && (!m_wasStopped || gen != m_seenGen)) consoleStop();
    m_wasStopped = stopped;
    m_seenGen = gen;
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
    for (int i = 0; i < 40 && !msdosdebug_stopped(m_dbg); ++i) QThread::msleep(1);
    resync();
}

void DebuggerWindow::attachAndRefresh()
{
    msdosdebug_attach(m_dbg);   /* stops the machine */
    m_attachedRunning = msdossession_is_running(m_session) != 0;
    m_wasStopped = false;
    settleAndSync();
}

void DebuggerWindow::tick()
{
    if (!isVisible()) return;
    if (!msdossession_is_running(m_session)) {
        if (m_attachedRunning) {
            m_attachedRunning = false;
            refreshStatus();
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
    const unsigned gen = msdosdebug_generation(m_dbg);
    const bool stopped = msdosdebug_stopped(m_dbg) != 0;
    if (gen != m_seenGen || stopped != m_wasStopped) {
        resync();
    } else if (!stopped && ++m_runningTicks >= 5) {
        /* live values twice a second while the machine runs */
        const int page = m_tabs->currentIndex();
        m_runningTicks = 0;
        refreshStatus();
        if (page == TAB_CPU) refreshCpu();
        if (page == TAB_INTS) refreshInts(false);
        if (page == TAB_FUJINET) refreshFujinet(false);
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
    const QString t = cleanText(text);
    if (t.isEmpty()) return;
    consoleAppend(t.endsWith(QLatin1Char('\n')) ? t : t + QLatin1Char('\n'));
}

/* At a stop the console shows where, as DEBUG's R does. */
void DebuggerWindow::consoleStop()
{
    static char out[4096];
    char reason[160];
    if (!msdossession_is_running(m_session)) return;
    msdosdebug_stop_reason(m_dbg, reason, sizeof reason);
    if (reason[0]) consoleOutput(QStringLiteral("— ") + QString::fromUtf8(reason));
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
    m_historyPos = (int)m_history.size();
    /* a command brings the reader back down */
    m_conOut->verticalScrollBar()->setValue(m_conOut->verticalScrollBar()->maximum());
    consoleAppend(QLatin1Char('-') + cmd + QLatin1Char('\n'));
    m_conIn->clear();

    out[0] = '\0';
    msdosdebug_command(m_dbg, cmd.toUtf8().constData(), out, sizeof out);
    consoleOutput(QString::fromUtf8(out));
    /* a t, p or g that stops again reports where, as resync sees the stop */
    m_wasStopped = false;
    settleAndSync();
}

void DebuggerWindow::showHistory()
{
    m_conIn->setText(m_historyPos < (int)m_history.size() ? m_history.at(m_historyPos) : QString());
    m_conIn->end(false);
}

/* Messages from Load Symbols land in the console and, since it may not be
 * the tab showing, the status bar. */
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
    if (msdosdebug_stopped(m_dbg)) msdosdebug_resume(m_dbg);
    else msdosdebug_stop(m_dbg);
    settleAndSync();
}

void DebuggerWindow::stepAnd(int (*fn)(msdosdebug *))
{
    fn(m_dbg);
    settleAndSync();
}

void DebuggerWindow::runToCursor()
{
    if (m_selected < 0) {
        message(QStringLiteral("Select a line in the Disassembly to run to"));
        return;
    }
    msdosdebug_run_to(m_dbg, (uint32_t)m_selected);
    settleAndSync();
}

void DebuggerWindow::toggleBreakpoint(uint32_t linear)
{
    if (msdosdebug_bp_toggle(m_dbg, linear) < 0)
        message(QStringLiteral("No room for another breakpoint"));
    resync();
}

/* seg:off, a register pair, a linear address or a symbol. */
bool DebuggerWindow::parseAddr(const QString &text, uint16_t *seg, uint32_t *off, uint32_t *lin)
{
    const QByteArray t = text.trimmed().toUtf8();
    if (t.isEmpty()) return false;
    return msdosdebug_parse_address(m_dbg, t.constData(), seg, off, lin) == 0;
}

void DebuggerWindow::setReg(int i, bool segment)
{
    QLineEdit *e = segment ? m_sreg[i] : m_reg[i];
    /* the 32-bit name when the 32-bit value shows */
    const char *name = segment ? kSregs[i] : m_is386 ? kRegs32[i] : kRegs16[i];
    long v;
    if (!parseNum(e->text(), &v))
        message(QStringLiteral("Not a number (hex, or # decimal)"));
    else if (!msdosdebug_stopped(m_dbg))
        message(QStringLiteral("Stop the PC first"));
    else if (msdosdebug_set_reg(m_dbg, name, (uint32_t)v) != 0)
        message(QStringLiteral("That register cannot be set here"));
    resync();
}

/* Go to: the dump from just above an address (seg:off, a register pair, a
 * linear address or a symbol), its row marked and scrolled to. */
void DebuggerWindow::memGoto()
{
    uint16_t seg;
    uint32_t off, lin;
    if (!parseAddr(m_memGoto->text(), &seg, &off, &lin)) {
        m_memMark = -1;
        refreshMem();
        message(QStringLiteral("Not an address (seg:off, ds:si, a linear address, a symbol)"));
        return;
    }
    m_memMark = lin;
    m_memTop = (lin > 0x100 ? lin - 0x100 : 0) & ~0xFu;
    refreshMem();
    const int line = 1 + (int)((lin - m_memTop) / 16);
    const int shown = m_mem->viewport()->height() / qMax(1, m_mem->fontMetrics().lineSpacing());
    m_mem->verticalScrollBar()->setValue(qMax(0, line - shown * 3 / 10));
}

void DebuggerWindow::memWrite()
{
    uint16_t seg;
    uint32_t off, lin;
    long v;
    if (!parseAddr(m_memAddr->text(), &seg, &off, &lin)) {
        message(QStringLiteral("Not an address"));
        return;
    }
    if (!parseNum(m_memVal->text(), &v) || v < 0 || v > 0xFF) {
        message(QStringLiteral("The value is a byte (00-FF)"));
        return;
    }
    const uint8_t byte = (uint8_t)v;
    msdosdebug_write(m_dbg, lin, &byte, 1);
    resync();
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
 * The end is optional; AH only means something for an INT. */
void DebuggerWindow::addBreakpoint()
{
    int type = m_bpType->currentIndex();
    if (type < 0 || type >= MSDOS_BP_TYPES) type = MSDOS_BP_EXEC;
    const QString start = m_bpStart->text().trimmed(), end = m_bpEnd->text().trimmed(), ahs = m_bpAh->text().trimmed();
    uint32_t a, b;
    long ah = -1;
    if (type <= MSDOS_BP_ACCESS) {
        uint16_t seg;
        uint32_t off;
        if (!parseAddr(start, &seg, &off, &a)) {
            m_bpMsg->setText(QStringLiteral("Not an address (seg:off, a linear address, a symbol)"));
            return;
        }
        b = a;
        if (!end.isEmpty() && !parseAddr(end, &seg, &off, &b)) {
            m_bpMsg->setText(QStringLiteral("The end is not an address"));
            return;
        }
    } else {
        long v, e;
        if (!parseNum(start, &v) || v < 0 || v > (type == MSDOS_BP_INT ? 0xFF : 0xFFFF)) {
            m_bpMsg->setText(type == MSDOS_BP_INT ? QStringLiteral("An interrupt is 00-FF")
                                                  : QStringLiteral("A port is 0000-FFFF"));
            return;
        }
        e = v;
        if (!end.isEmpty() && type != MSDOS_BP_INT && (!parseNum(end, &e) || e < v || e > 0xFFFF)) {
            m_bpMsg->setText(QStringLiteral("The last port is a port at or after the first"));
            return;
        }
        a = (uint32_t)v;
        b = (uint32_t)e;
        if (type == MSDOS_BP_INT && !ahs.isEmpty() && (!parseNum(ahs, &ah) || ah < 0 || ah > 0xFF)) {
            m_bpMsg->setText(QStringLiteral("AH is 00-FF, or empty for any"));
            return;
        }
    }
    if (b < a) {
        m_bpMsg->setText(QStringLiteral("The end is before the start"));
        return;
    }
    if (msdosdebug_bp_add(m_dbg, type, a, b, (int)ah) < 0) {
        m_bpMsg->setText(QStringLiteral("No room for another breakpoint"));
        return;
    }
    m_bpMsg->clear();
    m_bpStart->clear();
    m_bpEnd->clear();
    m_bpAh->clear();
    resync();
}

void DebuggerWindow::bpTypeChanged(int type)
{
    m_bpAh->setEnabled(type == MSDOS_BP_INT);
    m_bpEnd->setEnabled(type != MSDOS_BP_INT);
    m_bpStart->setPlaceholderText(type <= MSDOS_BP_ACCESS ? QStringLiteral("1234:0010 / symbol")
                                  : type == MSDOS_BP_INT ? QStringLiteral("21") : QStringLiteral("2F8"));
    m_bpEnd->setPlaceholderText(type <= MSDOS_BP_ACCESS ? QStringLiteral("(end)") : QStringLiteral("(last port)"));
}

void DebuggerWindow::loadSymbols()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Load Symbols (MS LINK or Watcom WLINK .map)"),
                                                      QString(), QStringLiteral("Linker maps (*.map *.MAP);;All files (*)"));
    if (path.isEmpty()) return;
    long seg = 0;
    if (!parseNum(m_symSeg->text(), &seg) || seg < 0 || seg > 0xFFFF) seg = 0;
    char msg[512] = "";
    if (msdosdebug_load_symbols(m_dbg, path.toLocal8Bit().constData(), (uint16_t)seg, msg, sizeof msg) < 0 && !msg[0])
        qstrncpy(msg, "No symbols found", sizeof msg);
    say(QString::fromUtf8(msg));
    resync();
}

bool DebuggerWindow::eventFilter(QObject *obj, QEvent *e)
{
    /* Up and Down walk the history */
    if (obj == m_conIn && e->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(e);
        if (ke->key() == Qt::Key_Up) {
            if (m_historyPos > 0) {
                --m_historyPos;
                showHistory();
            }
            return true;
        }
        if (ke->key() == Qt::Key_Down) {
            if (m_historyPos < (int)m_history.size()) {
                ++m_historyPos;
                showHistory();
            }
            return true;
        }
    }
    if (m_disasm && obj == m_disasm->viewport()) {
        /* A click selects a line (Run to Cursor); a click on its left edge,
         * or a double-click, toggles its breakpoint. */
        if (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) {
            auto *me = static_cast<QMouseEvent *>(e);
            if (me->button() != Qt::LeftButton) return true;
            const QTextCursor cur = m_disasm->cursorForPosition(me->position().toPoint());
            const int line = cur.blockNumber();
            if (line < 0 || line >= (int)m_lineLin.size()) return true;
            const bool gutter = cur.positionInBlock() <= 2;
            const bool dbl = e->type() == QEvent::MouseButtonDblClick;
            m_selected = m_lineLin[(size_t)line];
            if (gutter ? !dbl : dbl) toggleBreakpoint(m_lineLin[(size_t)line]);
            else refreshDisasm();
            return true;
        }
        if (e->type() == QEvent::MouseButtonRelease || e->type() == QEvent::MouseMove) return true;
        if (e->type() == QEvent::Wheel) {
            m_wheel += static_cast<QWheelEvent *>(e)->angleDelta().y();
            const int rows = -m_wheel / 40;     /* a notch is three rows */
            if (rows != 0 && msdossession_is_running(m_session)) {
                m_wheel += rows * 40;
                m_followPc->setChecked(false);
                m_topOff = rows > 0 ? rowsOn(m_topSeg, m_topOff, rows) : rowsBack(m_topSeg, m_topOff, -rows);
                refreshDisasm();
            }
            return true;
        }
        if (e->type() == QEvent::Resize && isVisible() && msdossession_is_running(m_session)) refreshDisasm();
    }
    return QMainWindow::eventFilter(obj, e);
}

/* The family's keys: F5 run/stop, F7 step, F8 step over, Shift+F8 step out. */
bool isDebuggerKey(const QKeyEvent *e)
{
    const Qt::KeyboardModifiers mods = e->modifiers() & ~Qt::KeypadModifier;
    switch (e->key()) {
    case Qt::Key_F5: case Qt::Key_F7: return mods == Qt::NoModifier;
    case Qt::Key_F8: return mods == Qt::NoModifier || mods == Qt::ShiftModifier;
    default: return false;
    }
}

/* These keys are the debugger's here, taken as plain key presses ahead of
 * any application-wide shortcut. */
bool DebuggerWindow::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride && isDebuggerKey(static_cast<QKeyEvent *>(e))) {
        e->accept();
        return true;
    }
    return QMainWindow::event(e);
}

void DebuggerWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_F1 && !(e->modifiers() & ~Qt::KeypadModifier)) {
        msdosHelpFor(this);
        return;
    }
    if (!isDebuggerKey(e)) {
        QMainWindow::keyPressEvent(e);
        return;
    }
    /* held, the steps repeat; Run/Stop does not */
    if (e->isAutoRepeat() && e->key() == Qt::Key_F5) return;
    switch (e->key()) {
    case Qt::Key_F5: toggleRun(); break;
    case Qt::Key_F7: stepAnd(msdosdebug_step); break;
    default:
        stepAnd((e->modifiers() & Qt::ShiftModifier) ? msdosdebug_step_out : msdosdebug_step_over);
        break;
    }
}

/* Hidden by Ctrl+Shift+D, the close button or the main window: the engine
 * goes away and the machine runs on. A minimised window's spontaneous hide
 * is not a detach. */
void DebuggerWindow::hideEvent(QHideEvent *e)
{
    if (!e->spontaneous()) {
        msdosdebug_detach(m_dbg);
        m_attachedRunning = false;
    }
    QMainWindow::hideEvent(e);
}

} // namespace

void msdosDebuggerShow(QWidget *parent, msdossession *session)
{
    if (!s_win) s_win = new DebuggerWindow(session, parent);
    s_win->show();
    s_win->raise();
    s_win->activateWindow();
    s_win->attachAndRefresh();
}

void msdosDebuggerToggle(QWidget *parent, msdossession *session)
{
    if (s_win && s_win->isVisible()) s_win->hide();
    else msdosDebuggerShow(parent, session);
}
