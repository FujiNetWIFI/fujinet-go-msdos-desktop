/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The keyboard is painted from the session's table (msdossession_keyboard_
 * layout), so every frontend shows the same keys for the machine -- the XT's
 * 83 keys or the enhanced 101 -- and the ones a desktop keeps for itself
 * (SysRq, Pause, Scroll Lock) are there too. A key is exactly w x h key
 * units of kKeyUnit pixels; the caps are drawn inside with a small gap, so no
 * theme padding changes the size.
 *
 * Keys and controls act on mouse press AND release: a key is HELD while the
 * mouse is down on it. Dragging off a key releases it; losing the focus and
 * hiding let go of what the mouse holds here (the main window releases its
 * own keys). What the PC sees held -- host keyboard and this panel -- is lit
 * in the accent, as are the joysticks' buttons a gamepad holds.
 *
 * FIXED SIZE, and a dialog-type window, so a tiling compositor floats it. A
 * singleton, hidden rather than destroyed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

#include "../FujiNetWindows.h"

namespace {

constexpr int kKeyUnit = 34;    /* pixels per key unit */
constexpr int kKeyGap = 3;      /* between key caps */
constexpr int kStick = 120;     /* a joystick's square */
constexpr int kPadNameW = 280;
constexpr int kMaxPads = 8;

const QString kGray = QStringLiteral("color: gray;");

QFont scaledFont(QFont f, qreal scale, bool bold = false)
{
    f.setPointSizeF((f.pointSizeF() > 0 ? f.pointSizeF() : 10) * scale);
    f.setBold(bold);
    return f;
}

/* Lines centred in r, the font shrunk until they fit; a long name breaks
 * at its spaces first. */
void drawFitted(QPainter &p, const QRectF &r, QString text, QFont f)
{
    const qreal start = f.pointSizeF();
    for (int pass = 0; pass < 2; ++pass) {
        const QStringList lines = text.split(QLatin1Char('\n'));
        f.setPointSizeF(start);
        for (;;) {
            const QFontMetricsF fm(f);
            qreal w = 0;
            for (const QString &l : lines) w = qMax(w, fm.horizontalAdvance(l));
            if ((w <= r.width() && fm.lineSpacing() * lines.size() <= r.height() + 2) || f.pointSizeF() <= 5.5)
                break;
            f.setPointSizeF(f.pointSizeF() - 0.5);
        }
        if (pass == 1 || f.pointSizeF() >= start * 0.8 || !text.contains(QLatin1Char(' ')))
            break;
        text.replace(QLatin1Char(' '), QLatin1Char('\n'));
    }
    p.setFont(f);
    p.drawText(r, Qt::AlignCenter, text);
}

/* ---- KeyboardWidget --------------------------------------------------------- */

class KeyboardWidget : public QWidget {
public:
    KeyboardWidget(msdossession *session, std::function<void(int, bool)> onKey)
        : m_session(session), m_onKey(std::move(onKey))
    {
        setFocusPolicy(Qt::NoFocus);
    }

    /* The machine changed: its keys. Copied, since the session rebuilds its
     * table when the machine changes. */
    void load()
    {
        const msdos_kbd_key *keys = nullptr;
        const int n = msdossession_keyboard_layout(m_session, &keys);
        m_keys.clear();
        m_labels.clear();
        float cols = 0, rows = 0;
        for (int i = 0; i < n; ++i) {
            m_keys.push_back(keys[i]);
            m_labels.push_back({ QString::fromUtf8(keys[i].label ? keys[i].label : ""),
                                 keys[i].shifted ? QString::fromUtf8(keys[i].shifted) : QString() });
            cols = qMax(cols, keys[i].x + keys[i].w);
            rows = qMax(rows, keys[i].y + keys[i].h);
        }
        m_held.assign(m_keys.size(), 0);
        m_pressed = -1;
        setFixedSize(qCeil(cols * kKeyUnit), qCeil(rows * kKeyUnit));
        update();
    }

    /* What the PC sees held (and the key the mouse holds); repaints only on a
     * change. */
    void pollHeld()
    {
        bool changed = false;
        for (int i = 0; i < (int)m_keys.size(); ++i) {
            const char on = i == m_pressed || msdossession_keyboard_held(m_session, i);
            if (on != m_held[(size_t)i]) {
                m_held[(size_t)i] = on;
                changed = true;
            }
        }
        if (changed) update();
    }

    /* Lets go of the key the mouse holds. */
    void release()
    {
        if (m_pressed < 0) return;
        const int i = m_pressed;
        m_pressed = -1;
        m_onKey(i, false);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        const QFont keyFont = scaledFont(font(), 0.75, true);
        const QFont smallFont = scaledFont(font(), 0.65);
        for (int i = 0; i < (int)m_keys.size(); ++i) {
            const QRectF r = keyRect(i);
            const bool held = m_held[(size_t)i] != 0;
            p.setPen(pal.color(QPalette::Mid));
            p.setBrush(held ? msdosAccentColor() : pal.color(QPalette::Button));
            p.drawRoundedRect(r, 4, 4);
            p.setPen(held ? msdosAccentInk() : pal.color(QPalette::ButtonText));
            const QRectF in = r.adjusted(2, 1, -2, -1);
            const auto &l = m_labels[(size_t)i];
            if (!l.second.isEmpty()) {
                const QRectF top(in.x(), in.y(), in.width(), in.height() / 2);
                const QRectF bottom(in.x(), in.y() + in.height() / 2, in.width(), in.height() / 2);
                drawFitted(p, top, l.second, smallFont);
                drawFitted(p, bottom, l.first, keyFont);
            } else if (!l.first.isEmpty()) {
                drawFitted(p, in, l.first, keyFont);
            }
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int i = keyAt(e->position());
        if (i < 0) return;
        release();
        m_pressed = i;
        m_onKey(i, true);
        pollHeld();
    }

    /* Dragging off a key must release it. */
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_pressed >= 0 && !keyRect(m_pressed).contains(e->position())) {
            release();
            pollHeld();
        }
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        release();
        pollHeld();
    }

private:
    QRectF keyRect(int i) const
    {
        const msdos_kbd_key &k = m_keys[(size_t)i];
        return QRectF(k.x * kKeyUnit + kKeyGap / 2.0, k.y * kKeyUnit + kKeyGap / 2.0, k.w * kKeyUnit - kKeyGap,
                      k.h * kKeyUnit - kKeyGap);
    }
    int keyAt(const QPointF &pos) const
    {
        for (int i = 0; i < (int)m_keys.size(); ++i)
            if (keyRect(i).contains(pos)) return i;
        return -1;
    }

    msdossession *m_session;
    std::function<void(int, bool)> m_onKey;
    std::vector<msdos_kbd_key> m_keys;
    std::vector<std::pair<QString, QString>> m_labels;     /* label, shifted */
    std::vector<char> m_held;
    int m_pressed = -1;
};

/* ---- StickWidget ------------------------------------------------------------- */

/* A joystick: dragged, it sends its position; let go, it springs back to the
 * middle. Between drags it shows where the PC's joystick is (a gamepad's). */
class StickWidget : public QWidget {
public:
    explicit StickWidget(std::function<void(int, int)> onMove) : m_onMove(std::move(onMove))
    {
        setFixedSize(kStick, kStick);
        setFocusPolicy(Qt::NoFocus);
    }
    bool dragging() const { return m_dragging; }
    void setPosition(int x, int y)
    {
        if (x == m_x && y == m_y) return;
        m_x = x;
        m_y = y;
        update();
    }
    void letGo()
    {
        if (!m_dragging) return;
        m_dragging = false;
        m_x = m_y = 0;
        m_onMove(0, 0);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QColor fg = palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText);
        const double w = width(), h = height();
        fg.setAlphaF(0.15);
        p.fillRect(QRectF(0.5, 0.5, w - 1, h - 1), fg);
        fg.setAlphaF(0.4);
        p.setPen(QPen(fg, 1));
        p.drawLine(QPointF(w / 2, 0), QPointF(w / 2, h));
        p.drawLine(QPointF(0, h / 2), QPointF(w, h / 2));
        const double cx = w / 2 + m_x / 32768.0 * (w / 2 - 12), cy = h / 2 + m_y / 32768.0 * (h / 2 - 12);
        fg.setAlphaF(0.8);
        p.setPen(QPen(fg, 1));
        p.setBrush(msdosAccentColor());
        p.drawEllipse(QPointF(cx, cy), 10, 10);
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_dragging = true;
        dragTo(e->position());
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_dragging) dragTo(e->position());
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) letGo();
    }

private:
    void dragTo(const QPointF &at)
    {
        const double w = width(), h = height();
        m_x = (int)qBound(-32768.0, (at.x() - w / 2) / (w / 2) * 32767, 32767.0);
        m_y = (int)qBound(-32768.0, (at.y() - h / 2) / (h / 2) * 32767, 32767.0);
        m_onMove(m_x, m_y);
        update();
    }

    std::function<void(int, int)> m_onMove;
    int m_x = 0, m_y = 0;
    bool m_dragging = false;
};

/* ---- PadButton --------------------------------------------------------------- */

/* A joystick button: press and release, not "clicked". */
class PadButton : public QPushButton {
public:
    PadButton(const QString &face, std::function<void(bool)> onPress)
        : QPushButton(face), m_onPress(std::move(onPress))
    {
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(32, 26);
    }
    void setHeld(bool held)
    {
        if (held == m_held) return;
        m_held = held;
        setStyleSheet(held ? msdosAccentStyle() : QString());
    }
    void letGo()
    {
        if (!m_down) return;
        m_down = false;
        m_onPress(false);
    }

protected:
    /* Not QPushButton's own: its pressed look would hide the held fill. */
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && !m_down) {
            m_down = true;
            m_onPress(true);
        }
    }
    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) letGo();
    }
    void leaveEvent(QEvent *e) override
    {
        letGo();
        QPushButton::leaveEvent(e);
    }

private:
    std::function<void(bool)> m_onPress;
    bool m_down = false;
    bool m_held = false;
};

/* ---- ControllersWindow ------------------------------------------------------ */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(msdossession *session, QWidget *parent);
    void present();
    void refresh();
    MsdosKeyHandler onKey;

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;
    /* Tab is the PC's Tab, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    QWidget *buildStick(int i);
    void rebuild();
    void rebuildPads();
    void sendStick(int i);
    void letGo();
    void tick();

    msdossession *m_session;
    KeyboardWidget *m_keyboard = nullptr;
    QLabel *m_lights = nullptr;
    QWidget *m_sticks = nullptr;
    QLabel *m_stickNote = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    struct Stick {
        StickWidget *area = nullptr;
        PadButton *buttons[4] = {};
        int x = 0, y = 0;
        unsigned held = 0;          /* the buttons the mouse holds */
    } m_stick[MSDOS_JOYSTICKS];
    QTimer m_timer;
    unsigned m_padGen = ~0u;
    int m_builtMachine = -1;
    int m_builtFitted = -1;
};

ControllersWindow::ControllersWindow(msdossession *session, QWidget *parent)
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);
    m_keyboard = new KeyboardWidget(m_session, [this](int i, bool down) {
        msdossession_keyboard_press(m_session, i, down);
    });
    root->addWidget(m_keyboard, 0, Qt::AlignHCenter);
    m_lights = new QLabel;
    m_lights->setStyleSheet(kGray);
    m_lights->setAlignment(Qt::AlignCenter);
    root->addWidget(m_lights);

    m_sticks = new QWidget;
    auto *sticks = new QHBoxLayout(m_sticks);
    sticks->setContentsMargins(0, 0, 0, 0);
    sticks->setSpacing(32);
    sticks->addStretch(1);
    for (int i = 0; i < MSDOS_JOYSTICKS; ++i)
        sticks->addWidget(buildStick(i));
    sticks->addStretch(1);
    root->addWidget(m_sticks);
    m_stickNote = new QLabel;
    m_stickNote->setStyleSheet(kGray);
    m_stickNote->setAlignment(Qt::AlignCenter);
    root->addWidget(m_stickNote);

    auto *padTitle = new QLabel(QStringLiteral("Gamepads"));
    QFont bold = padTitle->font();
    bold.setBold(true);
    padTitle->setFont(bold);
    padTitle->setAlignment(Qt::AlignCenter);
    root->addWidget(padTitle);
    m_padBox = new QVBoxLayout;
    m_padBox->setSpacing(6);
    root->addLayout(m_padBox);

    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    rebuild();
}

QWidget *ControllersWindow::buildStick(int i)
{
    Stick &s = m_stick[i];
    auto *box = new QWidget;
    auto *v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    auto *title = new QLabel(i ? QStringLiteral("Joystick B") : QStringLiteral("Joystick A"));
    QFont bold = title->font();
    bold.setBold(true);
    title->setFont(bold);
    title->setAlignment(Qt::AlignCenter);
    v->addWidget(title);
    s.area = new StickWidget([this, i](int x, int y) {
        m_stick[i].x = x;
        m_stick[i].y = y;
        sendStick(i);
    });
    v->addWidget(s.area, 0, Qt::AlignHCenter);
    auto *row = new QHBoxLayout;
    row->setSpacing(4);
    row->addStretch(1);
    for (int b = 0; b < 4; ++b) {
        s.buttons[b] = new PadButton(QString::number(b + 1), [this, i, b](bool down) {
            if (down) m_stick[i].held |= 1u << b;
            else m_stick[i].held &= ~(1u << b);
            sendStick(i);
        });
        row->addWidget(s.buttons[b]);
    }
    row->addStretch(1);
    v->addLayout(row);
    return box;
}

void ControllersWindow::sendStick(int i)
{
    msdossession_joystick(m_session, i, m_stick[i].x, m_stick[i].y, m_stick[i].held);
}

/* Lets go of everything the mouse holds in this panel. */
void ControllersWindow::letGo()
{
    m_keyboard->release();
    for (Stick &s : m_stick) {
        s.area->letGo();
        for (PadButton *b : s.buttons) b->letGo();
    }
}

void ControllersWindow::rebuild()
{
    letGo();
    m_keyboard->load();
    m_builtMachine = msdossession_machine(m_session);
    const bool fitted = msdossession_joystick_fitted(m_session) != 0;
    m_builtFitted = fitted;
    m_sticks->setEnabled(fitted);
    m_stickNote->setText(fitted ? QString() : QStringLiteral("No joystick on the game port (Preferences ▸ Ports)"));
    m_stickNote->setVisible(!fitted);
    m_padGen = msdossession_gamepad_generation(m_session);
    rebuildPads();
}

/* One row per pad: its name and the joystick it drives. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *w : m_padRows) delete w;
    m_padRows.clear();
    const int n = msdossession_gamepad_count(m_session);
    if (n == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads. One plugged in drives Joystick A, the next B."));
        none->setStyleSheet(kGray);
        none->setAlignment(Qt::AlignCenter);
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int i = 0; i < n && i < kMaxPads; ++i) {
        char name[128];
        msdossession_gamepad_name(m_session, i, name, sizeof name);
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel;
        label->setFixedWidth(kPadNameW);
        label->setText(label->fontMetrics().elidedText(QString::fromUtf8(name), Qt::ElideRight, kPadNameW));
        label->setToolTip(QString::fromUtf8(name));
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Joystick A"), QStringLiteral("Joystick B") });
        const int a = msdossession_gamepad_assignment(m_session, i);
        box->setCurrentIndex(a < 0 || a >= MSDOS_JOYSTICKS ? 0 : a + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, i](int idx) {
            msdossession_gamepad_assign(m_session, i, idx <= 0 ? -1 : idx - 1);
        });
        h->addWidget(label);
        h->addStretch(1);
        h->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
    }
}

void ControllersWindow::tick()
{
    m_keyboard->pollHeld();
    const unsigned lights = msdossession_lock_lights(m_session);
    m_lights->setText(QStringLiteral("%1 Num Lock     %2 Caps Lock     %3 Scroll Lock")
                          .arg(lights & 1 ? QStringLiteral("●") : QStringLiteral("○"),
                               lights & 2 ? QStringLiteral("●") : QStringLiteral("○"),
                               lights & 4 ? QStringLiteral("●") : QStringLiteral("○")));
    for (int i = 0; i < MSDOS_JOYSTICKS; ++i) {
        Stick &s = m_stick[i];
        int x, y;
        unsigned b;
        msdossession_joystick_state(m_session, i, &x, &y, &b);
        if (!s.area->dragging()) s.area->setPosition(x, y);
        for (int k = 0; k < 4; ++k) s.buttons[k]->setHeld(((b | s.held) >> k) & 1);
    }
    const unsigned gen = msdossession_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    }
    if (msdossession_machine(m_session) != m_builtMachine
        || (msdossession_joystick_fitted(m_session) != 0) != (m_builtFitted != 0))
        rebuild();
}

void ControllersWindow::present()
{
    if (msdossession_machine(m_session) != m_builtMachine) rebuild();
    m_padGen = msdossession_gamepad_generation(m_session);
    rebuildPads();
    show();
    raise();
    activateWindow();
}

void ControllersWindow::refresh()
{
    if (isVisible()) rebuild();
    else m_builtMachine = -1;   /* rebuilt when next shown */
}

void ControllersWindow::showEvent(QShowEvent *e)
{
    tick();
    m_timer.start(50);
    QWidget::showEvent(e);
}

void ControllersWindow::hideEvent(QHideEvent *e)
{
    m_timer.stop();
    letGo();
    QWidget::hideEvent(e);
}

/* Only what this panel's mouse holds: the main window releases its own. */
void ControllersWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && !isActiveWindow()) letGo();
    QWidget::changeEvent(e);
}

void ControllersWindow::keyPressEvent(QKeyEvent *e)
{
    if (!onKey || !onKey(e, true)) QWidget::keyPressEvent(e);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (!onKey || !onKey(e, false)) QWidget::keyReleaseEvent(e);
}

QPointer<ControllersWindow> g_window;

} // namespace

void msdosControllersToggle(QWidget *parent, msdossession *session, const MsdosKeyHandler &onKey)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    g_window->onKey = onKey;
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

bool msdosControllersVisible()
{
    return g_window && g_window->isVisible();
}

void msdosControllersRefresh()
{
    if (g_window) g_window->refresh();
}
