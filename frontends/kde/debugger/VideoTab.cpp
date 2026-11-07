/*
 * The debugger's Video tab (Qt6 Widgets).
 *
 * Everything that decides what to show lives in the core (msdosvidtab.h): this file is widgets.
 * The picture comes from the controller's cached buffer already opaque (0xFFRRGGBB), so
 * Format_RGB32 is safe on KWin (an alpha-0 picture would composite as see-through); a mouse
 * move redraws only the marks, never re-renders. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "VideoTab.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFlags>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

constexpr int kZoomValues[] = { 0, 1, 2, 3, 4, 6, 8, 12, 16 };
const char *const kZoomLabels[] = { "Fit", "1×", "2×", "3×", "4×", "6×", "8×", "12×", "16×" };
constexpr int kNZoom = 9;
constexpr int kPalCell = 18;

enum { SecPixel, SecRegs, SecPalette, SecMemory, SecTiming };

QLabel *monoLabel()
{
    auto *l = new QLabel;
    QFont f = l->font();
    f.setFamily(QStringLiteral("monospace"));
    f.setStyleHint(QFont::TypeWriter);
    l->setFont(f);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    return l;
}

} // namespace

/* ---- the picture ------------------------------------------------------------ */

class VideoPicture : public QWidget
{
public:
    explicit VideoPicture(VideoTab *owner) : m_owner(owner)
    {
        setMouseTracking(true);
        setMinimumSize(320, 200);
    }
    /* the controller's buffer, or a message when this view cannot be drawn */
    void sync()
    {
        int w = 0, h = 0;
        const uint32_t *px = msdosvid_tab_picture(m_owner->m_tab, &w, &h);
        if (px) {
            /* a view over the controller's buffer: valid until its next refresh */
            m_image = QImage(reinterpret_cast<const uchar *>(px), w, h, w * 4, QImage::Format_RGB32);
            m_msg.clear();
            setFixedSize(w, h);
        } else {
            m_image = QImage();
            const char *why = msdosvid_tab_why(m_owner->m_tab);
            m_msg = QString::fromUtf8(why ? why : "Nothing to show");
            setMinimumSize(320, 200);
            setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(18, 18, 24));
        if (m_image.isNull()) {
            p.setPen(QColor(140, 140, 148));
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_msg);
            return;
        }
        p.drawImage(0, 0, m_image);
        int hover[4] = { 0 }, pin[4] = { 0 };
        const int marks = msdosvid_tab_marks(m_owner->m_tab, hover, pin);
        if (marks & 1) {
            p.setPen(QPen(QColor(255, 255, 255, 230), 1));
            p.drawRect(hover[0], hover[1], hover[2] - 1, hover[3] - 1);
        }
        if (marks & 2) {
            p.setPen(QPen(QColor(255, 217, 26), 2));
            p.drawRect(pin[0] + 1, pin[1] + 1, pin[2] - 3, pin[3] - 3);
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (msdosvid_tab_hover(m_owner->m_tab, e->position().toPoint().x(), e->position().toPoint().y())) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void leaveEvent(QEvent *) override
    {
        if (msdosvid_tab_hover(m_owner->m_tab, -1, -1)) {
            update();
            m_owner->updatePixelSection();
        }
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        msdosvid_tab_pin(m_owner->m_tab, e->position().toPoint().x(), e->position().toPoint().y());
        update();
        m_owner->updatePixelSection();
    }
    void mouseDoubleClickEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        msdosvid_tab_hover(m_owner->m_tab, e->position().toPoint().x(), e->position().toPoint().y());
        const int id = msdosvid_tab_break_on_pixel(m_owner->m_tab);
        m_owner->note(id > 0 ? QStringLiteral("Breakpoint added: break on writes to that address (see Breakpoints)")
                             : QStringLiteral("That pixel has no CPU address to break on"));
        update();
        m_owner->updatePixelSection();
    }

private:
    VideoTab *m_owner;
    QImage m_image;
    QString m_msg;
};

/* ---- the palette ------------------------------------------------------------ */

class VideoPalette : public QWidget
{
public:
    explicit VideoPalette(VideoTab *owner) : m_owner(owner) { setMouseTracking(true); }
    int hovered() const { return m_hover; }
    void sync()
    {
        const msdosvid_pal_entry *e;
        const int n = msdosvid_tab_palette(m_owner->m_tab, &e);
        const int cols = n > 16 ? 16 : 8;
        const int rows = n ? (n + cols - 1) / cols : 1;
        setFixedSize(cols * (kPalCell + 2), rows * (kPalCell + 2));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const msdosvid_pal_entry *e;
        const int n = msdosvid_tab_palette(m_owner->m_tab, &e);
        const int cols = n > 16 ? 16 : 8;
        const int sel = msdosvid_tab_pixel_palette_index(m_owner->m_tab);
        QPainter p(this);
        for (int i = 0; i < n; ++i) {
            const QRect r((i % cols) * (kPalCell + 2), (i / cols) * (kPalCell + 2), kPalCell, kPalCell);
            p.fillRect(r, QColor::fromRgb(e[i].rgb & 0xFFFFFF));
            if (!e[i].active) p.fillRect(r, QColor(0, 0, 0, 140));
            if (e[i].age > 0 && e[i].age < MSDOSVID_REG_FADE) {
                p.setPen(QPen(QColor(255, 77, 230), 2));
                p.drawRect(r.adjusted(1, 1, -2, -2));
            }
            if (i == sel || i == m_hover) {
                p.setPen(QPen(Qt::white, 2));
                p.drawRect(r.adjusted(-1, -1, 1, 1));
            }
        }
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const msdosvid_pal_entry *pe;
        const int n = msdosvid_tab_palette(m_owner->m_tab, &pe);
        const int cols = n > 16 ? 16 : 8;
        int i = (e->position().toPoint().y() / (kPalCell + 2)) * cols + e->position().toPoint().x() / (kPalCell + 2);
        if (i < 0 || i >= n) i = -1;
        if (i != m_hover) {
            m_hover = i;
            update();
        }
        if (i < 0) {
            m_owner->m_palDetail->clear();
            return;
        }
        m_owner->m_palDetail->setText(QStringLiteral("#%1  %2\nused by %3%4%5")
                                          .arg(pe[i].rgb & 0xFFFFFF, 6, 16, QLatin1Char('0')).toUpper()
                                          .arg(QString::fromUtf8(pe[i].detail))
                                          .arg(pe[i].used >= 0 ? QString::number(pe[i].used) : QStringLiteral("?"))
                                          .arg(pe[i].used >= 0 ? QStringLiteral(" pixels") : QString())
                                          .arg(pe[i].active ? QString() : QStringLiteral("  (this mode cannot reach it)")));
    }
    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }

private:
    VideoTab *m_owner;
    int m_hover = -1;
};

/* ---- the tab ------------------------------------------------------------------ */

VideoTab::VideoTab(msdossession *session, QWidget *parent) : QWidget(parent)
{
    m_tab = msdosvid_tab_new(session);

    auto *root = new QHBoxLayout(this);
    auto *split = new QSplitter(Qt::Horizontal);
    root->addWidget(split);

    /* ---- left: controls, status, picture */
    auto *left = new QWidget;
    auto *lv = new QVBoxLayout(left);
    auto *bar = new QHBoxLayout;
    m_view = new QComboBox;
    for (int i = 0; i < MSDOSVID_V_COUNT; ++i) m_view->addItem(QString::fromUtf8(msdosvid_view_name(i)));
    m_view->setToolTip(QStringLiteral(
        "Decoded: built from VRAM and the registers now.\nDisplay: the last finished frame (what the monitor shows).\n"
        "Beam: the live frame, dimmed below the beam.\nValues: false colour by the value in VRAM.\n"
        "Bit planes: the EGA/VGA planes, 0 1 over 2 3.\nCharacter set, Memory map: the font and VRAM."));
    m_zoom = new QComboBox;
    for (int i = 0; i < kNZoom; ++i) m_zoom->addItem(QString::fromUtf8(kZoomLabels[i]));
    bar->addWidget(new QLabel(QStringLiteral("View")));
    bar->addWidget(m_view);
    bar->addWidget(new QLabel(QStringLiteral("Zoom")));
    bar->addWidget(m_zoom);
    bar->addStretch(1);
    lv->addLayout(bar);

    auto *ovGrid = new QGridLayout;
    int n = 0;
    const msdosvid_overlay_def *od = msdosvid_overlays(&n);
    for (int i = 0; i < n; ++i) {
        m_ov[i] = new QCheckBox(QString::fromUtf8(od[i].label));
        m_ov[i]->setToolTip(QString::fromUtf8(od[i].tip));
        ovGrid->addWidget(m_ov[i], i / 5, i % 5);
    }
    lv->addLayout(ovGrid);

    m_status = monoLabel();
    lv->addWidget(m_status);

    m_scroll = new QScrollArea;
    m_pic = new VideoPicture(this);
    m_scroll->setWidget(m_pic);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    lv->addWidget(m_scroll, 1);
    m_note = new QLabel;
    lv->addWidget(m_note);
    split->addWidget(left);

    /* ---- right: sections */
    m_sections = new QTabWidget;
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_pixelText = monoLabel();
        m_pixelText->setWordWrap(true);
        v->addWidget(m_pixelText);
        auto *btns = new QHBoxLayout;
        auto *brk = new QPushButton(QStringLiteral("Break on its writes"));
        auto *unpin = new QPushButton(QStringLiteral("Unpin"));
        btns->addWidget(brk);
        btns->addWidget(unpin);
        btns->addStretch(1);
        v->addLayout(btns);
        v->addStretch(1);
        connect(brk, &QPushButton::clicked, this, [this] {
            const int id = msdosvid_tab_break_on_pixel(m_tab);
            note(id > 0 ? QStringLiteral("Breakpoint added: break on writes to that address (see Breakpoints)")
                        : QStringLiteral("Pin a pixel that has a CPU address first"));
        });
        connect(unpin, &QPushButton::clicked, this, [this] {
            msdosvid_tab_unpin(m_tab);
            m_pic->update();
            updatePixelSection();
        });
        m_sections->addTab(w, QStringLiteral("Pixel"));
    }
    m_regs = new QTableWidget(0, 4);
    m_regs->setHorizontalHeaderLabels({ QStringLiteral("Reg"), QStringLiteral("Name"), QStringLiteral("Value"), QStringLiteral("Meaning") });
    m_regs->verticalHeader()->hide();
    m_regs->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_regs->setSelectionMode(QAbstractItemView::NoSelection);
    m_regs->horizontalHeader()->setStretchLastSection(true);
    {
        QFont f = m_regs->font();
        f.setFamily(QStringLiteral("monospace"));
        f.setStyleHint(QFont::TypeWriter);
        m_regs->setFont(f);
    }
    m_sections->addTab(m_regs, QStringLiteral("Registers"));
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_pal = new VideoPalette(this);
        v->addWidget(m_pal, 0, Qt::AlignLeft | Qt::AlignTop);
        m_palDetail = monoLabel();
        v->addWidget(m_palDetail);
        v->addStretch(1);
        m_sections->addTab(w, QStringLiteral("Palette"));
    }
    m_memText = monoLabel();
    m_memText->setTextFormat(Qt::RichText);
    m_sections->addTab(m_memText, QStringLiteral("Memory"));
    {
        auto *w = new QWidget;
        auto *v = new QVBoxLayout(w);
        m_beam = new QProgressBar;
        m_beam->setRange(0, 1000);
        m_beam->setFormat(QStringLiteral("beam"));
        v->addWidget(m_beam);
        m_timingText = monoLabel();
        v->addWidget(m_timingText);
        v->addStretch(1);
        m_sections->addTab(w, QStringLiteral("Timing"));
    }
    split->addWidget(m_sections);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    m_sections->setUsesScrollButtons(false);
    m_sections->setMinimumWidth(440);
    split->setSizes({ 700, 450 });

    /* start from the controller's state (the MSDOS_DEBUGGER_VIDEO_* variables) */
    m_updating = true;
    m_view->setCurrentIndex(msdosvid_tab_view(m_tab));
    for (int i = 0; i < kNZoom; ++i)
        if (kZoomValues[i] == msdosvid_tab_zoom(m_tab)) m_zoom->setCurrentIndex(i);
    for (int i = 0; i < n; ++i) m_ov[i]->setChecked((msdosvid_tab_overlays(m_tab) & od[i].bit) != 0);
    m_sections->setCurrentIndex(msdosvid_tab_env_section(m_tab));
    m_updating = false;

    connect(m_view, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating) return;
        msdosvid_tab_set_view(m_tab, i);
        refreshView(MSDOSVID_TAB_PICTURE | MSDOSVID_TAB_TABLES);
    });
    connect(m_zoom, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_updating || i < 0 || i >= kNZoom) return;
        msdosvid_tab_set_zoom(m_tab, kZoomValues[i]);
        refreshView(MSDOSVID_TAB_PICTURE);
    });
    for (int i = 0; i < n; ++i)
        connect(m_ov[i], &QCheckBox::toggled, this, [this, n, od](bool) {
            if (m_updating) return;
            unsigned m = 0;
            for (int k = 0; k < n; ++k)
                if (m_ov[k]->isChecked()) m |= od[k].bit;
            msdosvid_tab_set_overlays(m_tab, m);
            refreshView(MSDOSVID_TAB_PICTURE);
        });
    connect(m_sections, &QTabWidget::currentChanged, this, [this](int) { refreshView(MSDOSVID_TAB_TABLES); });
}

VideoTab::~VideoTab()
{
    msdosvid_tab_free(m_tab);
}

void VideoTab::note(const QString &text)
{
    m_note->setText(text);
}

void VideoTab::updatePixelSection()
{
    const msdosvid_pixel *p = msdosvid_tab_pixel(m_tab);
    m_pixelText->setText(p ? QString::fromUtf8(p->text)
                           : QStringLiteral("Move over the picture to inspect a pixel.\nClick to pin it; "
                                            "double-click to break on writes to it."));
    if (m_pal) m_pal->update();
}

void VideoTab::fillRegs()
{
    const msdosvid_reg *r;
    const int n = msdosvid_tab_regs(m_tab, &r);
    QString sig;
    for (int i = 0; i < n; ++i)
        sig += QString::number(r[i].value) + (r[i].age > 0 && r[i].age < MSDOSVID_REG_FADE ? QLatin1Char('*') : QLatin1Char('.'));
    sig += QString::number(n);
    if (sig == m_regsSig) return;
    m_regsSig = sig;
    const int vpos = m_regs->verticalScrollBar()->value();
    m_regs->setRowCount(n);
    for (int i = 0; i < n; ++i) {
        const QString id = r[i].port && r[i].index < 0
                               ? QStringLiteral("%1 %2").arg(QString::fromUtf8(r[i].group), QStringLiteral("%1").arg(r[i].port, 3, 16, QLatin1Char('0')).toUpper())
                               : QStringLiteral("%1 %2").arg(QString::fromUtf8(r[i].group), QStringLiteral("%1").arg(r[i].index, 2, 16, QLatin1Char('0')).toUpper());
        const QString val = QStringLiteral("%1").arg(r[i].value, r[i].digits, 16, QLatin1Char('0')).toUpper();
        const QString cols[4] = { id, QString::fromUtf8(r[i].name), val, QString::fromUtf8(r[i].meaning) };
        const bool hot = r[i].age > 0 && r[i].age < MSDOSVID_REG_FADE;
        for (int c = 0; c < 4; ++c) {
            auto *it = m_regs->item(i, c);
            if (!it) {
                it = new QTableWidgetItem;
                m_regs->setItem(i, c, it);
            }
            it->setText(cols[c]);
            it->setBackground(hot ? QBrush(QColor(122, 42, 106)) : QBrush());
            it->setForeground(hot ? QBrush(Qt::white) : QBrush());
        }
    }
    m_regs->resizeColumnsToContents();
    m_regs->verticalScrollBar()->setValue(vpos);
}

void VideoTab::fillPalette()
{
    m_pal->sync();
}

void VideoTab::fillMemory()
{
    const msdosvid_region *r;
    const int n = msdosvid_tab_regions(m_tab, &r);
    const msdos_vid_snap *sn = msdosvid_tab_snap(m_tab);
    auto hex5 = [](uint32_t v) { return QString::number(v, 16).toUpper().rightJustified(5, QLatin1Char('0')); };
    QString s = QStringLiteral("<pre>");
    if (!n) s += QStringLiteral("No video memory");
    for (int i = 0; i < n; ++i) {
        s += QStringLiteral("<span style=\"color:#%1\">■■</span> %2\n     VRAM %3 + %4")
                 .arg(QString::number(r[i].color & 0xFFFFFF, 16).rightJustified(6, QLatin1Char('0')),
                      QString::fromUtf8(r[i].name).toHtmlEscaped(), hex5(r[i].vram_off), hex5(r[i].size));
        if (r[i].cpu_addr != MSDOS_VID_NONE) s += QStringLiteral("   CPU %1").arg(hex5(r[i].cpu_addr));
        s += QLatin1Char('\n');
    }
    if (n)
        s += QStringLiteral("\n%1 KB of VRAM%2; the CPU sees it at %3-%4.\nSelect the Memory map view to see it all.")
                 .arg(sn->vram_size / 1024).arg(sn->planar ? QStringLiteral(", four planes") : QString(),
                                                hex5(sn->cpu_base), hex5(sn->cpu_base + (sn->cpu_size ? sn->cpu_size - 1 : 0)));
    s += QStringLiteral("</pre>");
    m_memText->setText(s);
}

void VideoTab::fillTiming()
{
    const char *const *lines;
    const int n = msdosvid_tab_timing(m_tab, &lines);
    QString s;
    for (int i = 0; i < n; ++i) {
        const QString line = QString::fromUtf8(lines[i]);
        const int t = line.indexOf(QLatin1Char('\t'));
        s += t >= 0 ? line.left(t).leftJustified(13) + QLatin1Char(' ') + line.mid(t + 1) : line;
        s += QLatin1Char('\n');
    }
    m_timingText->setText(s.isEmpty() ? QStringLiteral("The PC is off") : s);
    m_beam->setValue(msdosvid_tab_beam_permille(m_tab));
}

void VideoTab::refreshView(unsigned what)
{
    if (what & MSDOSVID_TAB_PICTURE) {
        const QSize vp = m_scroll->viewport()->size();
        if (vp.width() > 4 && vp.height() > 4) msdosvid_tab_set_viewport(m_tab, vp.width() - 2, vp.height() - 2);
        m_pic->sync();
    }
    if (what & MSDOSVID_TAB_TABLES) {
        m_status->setText(QString::fromUtf8(msdosvid_tab_status(m_tab)));
        switch (m_sections->currentIndex()) {
        case SecRegs: fillRegs(); break;
        case SecPalette: fillPalette(); break;
        case SecMemory: fillMemory(); break;
        case SecTiming: fillTiming(); break;
        default: break;
        }
        updatePixelSection();
    }
}

void VideoTab::refresh(bool force)
{
    unsigned what = msdosvid_tab_refresh(m_tab, force);
    if (force) what |= MSDOSVID_TAB_PICTURE | MSDOSVID_TAB_TABLES;
    refreshView(what);
}
