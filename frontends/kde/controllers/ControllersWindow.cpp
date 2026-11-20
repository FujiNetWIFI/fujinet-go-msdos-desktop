/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * Everything drawn comes from the session's tables (core/src/controls.c): the
 * keyboard skin of the CURRENT machine (msdossession_keyboard_skin_layout) and
 * the two Kraft sticks (msdossession_controls_layout), so every toolkit shows
 * the same parts. The window is sized once, for the largest of the three
 * keyboards plus the sticks, and never changes size: a smaller keyboard is
 * centred in the room.
 *
 * Keys and parts act on mouse press AND release: a key is HELD while the mouse
 * is down on it. A stick is steered by dragging anywhere on its plate and
 * springs back on release; button 1 is the knob, 2 the side pad, 3 and 4 the
 * pills. What the PC sees held (the host keyboard, this panel, a gamepad) is lit
 * in MSDOS_HELD_RGB: a 2 px outline and a 20% wash.
 *
 * Map mode: pick one of the sixteen targets (a button, a pill, or a side of a
 * stick's well), then press a key or a gamepad button for it.
 *
 * Painting is straight QPainter on widgets (no QImage), so Qt's Format_RGB32
 * alpha-0 trap does not apply here.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../help/HelpWindow.h"

namespace {

constexpr double kKeyUnit = 32.0;       /* pixels per keyboard unit */
constexpr double kStickUnit = 28.0;     /* pixels per joystick unit */
constexpr int kMargin = 3;              /* round each picture, room for outlines */
constexpr int kMaxPadRows = 3;
constexpr qreal kDimAlpha = 0.35;       /* a part the game port does not have */
constexpr int kHeldWashAlpha = 51;      /* 20% */

QColor rgbColor(uint32_t rgb, int alpha = 255)
{
    return QColor((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, alpha);
}

QString local(const char *text) { return QString::fromUtf8(text ? text : ""); }

QFont pixelFont(QFont base, double pixels, bool bold = false, bool italic = false)
{
    base.setPixelSize(qMax(5, (int)qRound(pixels)));
    base.setBold(bold);
    base.setItalic(italic);
    return base;
}

/* Lines centred in rect, the font shrunk until they fit. */
void drawFitted(QPainter &pnt, const QRectF &rect, const QString &text, QFont font)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF metrics(font);
        qreal wide = 0;
        for (const QString &line : lines) wide = qMax(wide, metrics.horizontalAdvance(line));
        const int size = font.pixelSize() > 0 ? font.pixelSize() : 10;
        if ((wide <= rect.width() && metrics.lineSpacing() * lines.size() <= rect.height() + 2) || size <= 5)
            break;
        font.setPixelSize(size - 1);
    }
    pnt.setFont(font);
    pnt.drawText(rect, Qt::AlignCenter, text);
}

/* The held light: a wash and a 2 px outline inside the shape. */
void paintHeld(QPainter &pnt, const QPainterPath &shape)
{
    pnt.save();
    pnt.setPen(Qt::NoPen);
    pnt.setBrush(rgbColor(MSDOS_HELD_RGB, kHeldWashAlpha));
    pnt.drawPath(shape);
    pnt.setClipPath(shape);
    pnt.setPen(QPen(rgbColor(MSDOS_HELD_RGB), 4));      /* the clip halves it to 2 px inside */
    pnt.setBrush(Qt::NoBrush);
    pnt.drawPath(shape);
    pnt.restore();
}

/* A key name as a legend: long names shortened. */
QString legendName(QString name)
{
    if (name.startsWith(QLatin1String("Keypad "))) return QStringLiteral("KP ") + name.mid(7);
    if (name.startsWith(QLatin1String("Left "))) return QStringLiteral("L ") + name.mid(5);
    if (name.startsWith(QLatin1String("Right "))) return QStringLiteral("R ") + name.mid(6);
    if (name == QLatin1String("Backspace")) return QStringLiteral("BkSp");
    if (name == QLatin1String("Escape")) return QStringLiteral("Esc");
    if (name == QLatin1String("Return")) return QStringLiteral("Ret");
    return name;
}

QString keyName(msdossession *session, int target)
{
    const msdos_binding binding = msdossession_binding_get(session, target);
    return binding.key ? local(msdossession_hid_name(binding.key)) : QString();
}

QString padNames(msdossession *session, int target)
{
    const msdos_binding binding = msdossession_binding_get(session, target);
    QString text;
    if (binding.button != MSDOS_PAD_BTN_NONE) text = local(msdos_pad_button_name(binding.button));
    if (binding.button2 != MSDOS_PAD_BTN_NONE) {
        if (!text.isEmpty()) text += QStringLiteral(", ");
        text += local(msdos_pad_button_name(binding.button2));
    }
    return text;
}

QString orNone(const QString &text) { return text.isEmpty() ? QStringLiteral("none") : text; }

/* ---- KeyboardWidget -------------------------------------------------------------- */

class KeyboardWidget : public QWidget {
public:
    explicit KeyboardWidget(msdossession *session) : m_session(session)
    {
        setFocusPolicy(Qt::NoFocus);
        /* sized for the largest of the three skins, whatever machine is selected */
        float widest = 0, tallest = 0;
        for (int machine = 0; machine < MSDOS_M_COUNT; ++machine) {
            int count;
            float width, height;
            if (!msdossession_keyboard_skin_layout(machine, &count, &width, &height)) continue;
            widest = qMax(widest, width);
            tallest = qMax(tallest, height);
        }
        setFixedSize(qCeil(widest * kKeyUnit) + 2 * kMargin, qCeil(tallest * kKeyUnit) + 2 * kMargin);
        load();
    }

    /* The selected machine's skin (static tables; valid for the process). */
    void load()
    {
        releaseKey();
        m_machine = msdossession_machine(m_session);
        m_items = msdossession_keyboard_skin_layout(m_machine, &m_count, &m_width, &m_height);
        m_lights = ~0u;
        m_held.assign((size_t)m_count, 0);
        update();
    }
    int machine() const { return m_machine; }

    /* Lets go of the key the mouse holds. */
    void releaseKey()
    {
        if (m_pressed < 0) return;
        const int key = m_pressed;
        m_pressed = -1;
        msdossession_keyboard_press(m_session, key, 0);
    }

    /* What the PC sees held (and the key the mouse holds); repaints only on a change. */
    void pollHeld()
    {
        bool changed = false;
        const unsigned lights = msdossession_lock_lights(m_session);
        if (lights != m_lights) {
            m_lights = lights;
            changed = true;
        }
        for (int idx = 0; idx < m_count; ++idx) {
            if (m_items[idx].kind != MSDOS_SKIN_KEYCAP) continue;
            const char on = m_items[idx].key == m_pressed || msdossession_keyboard_held(m_session, m_items[idx].key);
            if (on != m_held[(size_t)idx]) {
                m_held[(size_t)idx] = on;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        pnt.setRenderHint(QPainter::TextAntialiasing);
        const QPointF origin = originPoint();
        for (int idx = 0; idx < m_count; ++idx) {
            const msdos_skin_item &item = m_items[idx];
            const QRectF rect(origin.x() + item.x * kKeyUnit, origin.y() + item.y * kKeyUnit, item.w * kKeyUnit,
                              item.h * kKeyUnit);
            switch (item.kind) {
            case MSDOS_SKIN_BODY: paintBody(pnt, item, rect); break;
            case MSDOS_SKIN_KEYCAP: paintCap(pnt, item, rect, m_held[(size_t)idx] != 0); break;
            case MSDOS_SKIN_LED: paintLed(pnt, item, rect); break;
            }
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const int key = keyAt(evt->position());
        if (key < 0) return;
        releaseKey();
        m_pressed = key;
        msdossession_keyboard_press(m_session, key, 1);
        pollHeld();
    }

    /* Dragging off a key must release it. */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        if (m_pressed >= 0 && keyAt(evt->position()) != m_pressed) {
            releaseKey();
            pollHeld();
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        releaseKey();
        pollHeld();
    }

private:
    QPointF originPoint() const
    {
        return QPointF((width() - m_width * kKeyUnit) / 2, (height() - m_height * kKeyUnit) / 2);
    }

    /* The layout key under pos, or -1. */
    int keyAt(const QPointF &pos) const
    {
        const QPointF origin = originPoint();
        const int idx = msdossession_keyboard_skin_hit(m_machine, (float)((pos.x() - origin.x()) / kKeyUnit),
                                                       (float)((pos.y() - origin.y()) / kKeyUnit));
        return idx < 0 ? -1 : m_items[idx].key;
    }

    void paintBody(QPainter &pnt, const msdos_skin_item &item, const QRectF &rect) const
    {
        const QString name = local(item.label);
        if (name.endsWith(QLatin1String("Bracket"))) {
            /* the Fn / Alt outline round the keys it marks */
            pnt.setBrush(Qt::NoBrush);
            pnt.setPen(QPen(rgbColor(item.alt_rgb), 2));
            pnt.drawRoundedRect(rect, 5, 5);
            return;
        }
        if (name == QLatin1String("Logo")) {
            pnt.setPen(rgbColor(item.ink_rgb));
            QFont font = pixelFont(this->font(), rect.height() * 0.62, true, false);
            font.setFamily(QStringLiteral("serif"));
            pnt.setFont(font);
            pnt.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, local(item.shifted));
            return;
        }
        const bool wholeCase = name == QLatin1String("Case");
        pnt.setBrush(rgbColor(item.fill_rgb));
        pnt.setPen(QPen(rgbColor(item.fill_rgb).darker(wholeCase ? 130 : 108), 1));
        pnt.drawRoundedRect(rect, wholeCase ? 10 : 5, wholeCase ? 10 : 5);
        if (item.shifted && *item.shifted) {
            pnt.setPen(rgbColor(item.ink_rgb));
            drawFitted(pnt, rect.adjusted(3, 1, -3, -1), local(item.shifted), pixelFont(font(), rect.height() * 0.6, true));
        }
    }

    /* A key's outline in its case: the halves of an L-shaped key run into each other. */
    static QPainterPath capShape(const msdos_skin_item &item, const QRectF &rect)
    {
        constexpr qreal gap = 1.5;
        QRectF cap = rect.adjusted(gap, gap, -gap, -gap);
        const bool below = item.shape == MSDOS_SHAPE_JOIN_BELOW, above = item.shape == MSDOS_SHAPE_JOIN_ABOVE;
        if (below) cap.setBottom(rect.bottom());
        if (above) cap.setTop(rect.top());
        QPainterPath shape;
        shape.setFillRule(Qt::WindingFill);
        shape.addRoundedRect(cap, 5, 5);
        if (below || above) {
            /* square the joined edge so the two halves meet flat */
            QRectF square = cap;
            if (below) square.setTop(cap.bottom() - 8);
            else square.setBottom(cap.top() + 8);
            shape.addRect(square);
        }
        return shape;
    }

    QRectF itemRect(const msdos_skin_item &item) const
    {
        const QPointF origin = originPoint();
        return QRectF(origin.x() + item.x * kKeyUnit, origin.y() + item.y * kKeyUnit, item.w * kKeyUnit, item.h * kKeyUnit);
    }

    void paintCap(QPainter &pnt, const msdos_skin_item &item, const QRectF &rect, bool held) const
    {
        constexpr qreal gap = 1.5;
        QRectF cap = rect.adjusted(gap, gap, -gap, -gap);
        const bool below = item.shape == MSDOS_SHAPE_JOIN_BELOW, above = item.shape == MSDOS_SHAPE_JOIN_ABOVE;
        if (below) cap.setBottom(rect.bottom());
        if (above) cap.setTop(rect.top());
        const QPainterPath shape = capShape(item, rect);
        const QColor fill = rgbColor(item.fill_rgb);
        /* a darker edge below: the cap's depth (the lower half of a joined key has it, the upper has none) */
        pnt.setPen(Qt::NoPen);
        if (!below) {
            QPainterPath foot;
            foot.addRoundedRect(cap.translated(0, 2), 5, 5);
            pnt.setBrush(fill.darker(135));
            pnt.drawPath(foot);
        }
        pnt.setBrush(fill);
        pnt.setPen((below || above) ? Qt::NoPen : QPen(fill.darker(118), 1));
        pnt.drawPath(shape);

        pnt.setPen(rgbColor(item.ink_rgb));
        const QRectF inner = cap.adjusted(2, 1, -2, -1);
        const QString label = local(item.label), shifted = local(item.shifted), fnlegend = local(item.fn_legend);
        const double unit = qMin(cap.height(), 30.0);
        const QFont big = pixelFont(font(), qMin(unit * 0.36, 11.5), true), small = pixelFont(font(), qMin(unit * 0.3, 9.5));
        if (!fnlegend.isEmpty()) {
            const QRectF top(inner.x(), inner.y(), inner.width(), inner.height() * 0.55);
            const QRectF bottom(inner.x(), inner.y() + inner.height() * 0.55, inner.width(), inner.height() * 0.45);
            drawFitted(pnt, top, label, big);
            pnt.setPen(rgbColor(item.alt_rgb));
            drawFitted(pnt, bottom, fnlegend, small);
        } else if (!shifted.isEmpty()) {
            const QRectF top(inner.x(), inner.y(), inner.width(), inner.height() / 2);
            const QRectF bottom(inner.x(), inner.y() + inner.height() / 2, inner.width(), inner.height() / 2);
            drawFitted(pnt, top, shifted, small);
            drawFitted(pnt, bottom, label, big);
        } else if (!label.isEmpty()) {
            drawFitted(pnt, inner, label, big);
        }
        if (held && !below) {
            /* both halves of the PCjr's Enter light as one shape */
            QPainterPath whole = shape;
            if (above)
                for (int idx = 0; idx < m_count; ++idx)
                    if (m_items[idx].kind == MSDOS_SKIN_KEYCAP && m_items[idx].key == item.key
                        && m_items[idx].shape == MSDOS_SHAPE_JOIN_BELOW)
                        whole = whole.united(capShape(m_items[idx], itemRect(m_items[idx]))).simplified();
            paintHeld(pnt, whole);
        }
    }

    void paintLed(QPainter &pnt, const msdos_skin_item &item, const QRectF &rect) const
    {
        const bool lit = (m_lights >> item.key) & 1u;
        pnt.setPen(QPen(rgbColor(item.fill_rgb).darker(160), 1));
        pnt.setBrush(rgbColor(lit ? item.alt_rgb : item.fill_rgb));
        pnt.drawRoundedRect(rect, 2, 2);
        /* its name under it, small */
        pnt.setPen(rgbColor(item.ink_rgb));
        pnt.setFont(pixelFont(font(), 7));
        pnt.drawText(QRectF(rect.x() - 12, rect.bottom() + 1, rect.width() + 24, 9), Qt::AlignCenter, local(item.label));
    }

    msdossession *m_session;
    const msdos_skin_item *m_items = nullptr;
    int m_count = 0;
    float m_width = 0, m_height = 0;
    int m_machine = -1;
    std::vector<char> m_held;           /* per item */
    unsigned m_lights = ~0u;
    int m_pressed = -1;                 /* the layout key the mouse holds */
};

/* ---- SticksWidget ----------------------------------------------------------------- */

/* The two Kraft sticks. */
class SticksWidget : public QWidget {
public:
    SticksWidget(msdossession *session, std::function<void(int)> onPick)
        : m_session(session), m_onPick(std::move(onPick))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setFixedSize(qCeil(MSDOS_CONTROLS_W * kStickUnit) + 2 * kMargin, qCeil(MSDOS_CONTROLS_H * kStickUnit) + 2 * kMargin);
        syncMode();
    }

    /* The game port changed: its dimmed and live parts. */
    void syncMode()
    {
        const int mode = msdossession_game_port_mode(m_session);
        if (mode == m_mode && m_ctl) return;
        letGo();
        m_mode = mode;
        m_ctl = msdossession_controls_layout(mode, &m_count, nullptr, nullptr);
        update();
    }

    /* Lets go of everything the mouse holds. */
    void letGo()
    {
        const bool any = m_drag >= 0 || m_holdIdx >= 0;
        m_drag = m_holdIdx = -1;
        for (int stick = 0; stick < MSDOS_JOYSTICKS; ++stick) {
            m_state[stick] = State();
            msdossession_pointer_set(m_session, stick, 0, 0, 0);
        }
        if (any) update();
    }

    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setShowBindings(bool visible)
    {
        m_showBindings = visible;
        update();
    }

    /* What the PC sees of both sticks; repaints only on a change. */
    void pollHeld()
    {
        syncMode();
        bool changed = false;
        for (int stick = 0; stick < MSDOS_JOYSTICKS; ++stick) {
            Seen now;
            msdossession_joystick_state(m_session, stick, &now.xpos, &now.ypos, &now.buttons);
            now.buttons |= msdossession_buttons_held(m_session, stick);
            for (int act = 0; act < MSDOS_ACT_B1; ++act)
                if (msdossession_target_held(m_session, MSDOS_TARGET(stick, act))) now.dirs |= 1u << act;
            if (now.xpos != m_seen[stick].xpos || now.ypos != m_seen[stick].ypos || now.buttons != m_seen[stick].buttons
                || now.dirs != m_seen[stick].dirs) {
                m_seen[stick] = now;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter pnt(this);
        pnt.setRenderHint(QPainter::Antialiasing);
        pnt.setRenderHint(QPainter::TextAntialiasing);
        for (int idx = 0; idx < m_count; ++idx) {
            const msdos_control &ctl = m_ctl[idx];
            pnt.save();
            if (!ctl.enabled) pnt.setOpacity(kDimAlpha);
            switch (ctl.kind) {
            case MSDOS_CTL_CAPTION: paintCaption(pnt, ctl); break;
            case MSDOS_CTL_BODY: paintBody(pnt, ctl); break;
            case MSDOS_CTL_TRIM: paintTrim(pnt, ctl); break;
            case MSDOS_CTL_STICK: paintStick(pnt, ctl); break;
            case MSDOS_CTL_BUTTON:
            case MSDOS_CTL_PILL: paintButton(pnt, ctl); break;
            }
            pnt.restore();
        }
    }

    void mousePressEvent(QMouseEvent *evt) override
    {
        if (evt->button() != Qt::LeftButton) return;
        const int idx = hitAt(evt->position());
        if (m_map != -2) {
            if (idx >= 0) pickTarget(idx, evt->position());
            return;
        }
        if (idx < 0 || m_drag >= 0 || m_holdIdx >= 0) return;      /* one thing at a time */
        const msdos_control &ctl = m_ctl[idx];
        if (ctl.kind == MSDOS_CTL_STICK) {
            m_drag = idx;
            steer(ctl, evt->position());
        } else if (ctl.kind == MSDOS_CTL_BUTTON || ctl.kind == MSDOS_CTL_PILL) {
            m_holdIdx = idx;
            State &state = m_state[ctl.stick];
            state.buttons |= 1u << (ctl.target % MSDOS_ACT_COUNT - MSDOS_ACT_B1);
            send(ctl.stick);
        }
        update();
    }

    /* The stick follows the pointer, wherever it is; a button lets go when the
     * pointer leaves it. */
    void mouseMoveEvent(QMouseEvent *evt) override
    {
        if (m_drag >= 0) {
            steer(m_ctl[m_drag], evt->position());
        } else if (m_holdIdx >= 0) {
            if (hitAt(evt->position()) != m_holdIdx) letGo();
        } else {
            hover(evt->position());
        }
    }

    void mouseReleaseEvent(QMouseEvent *evt) override
    {
        if (evt->button() == Qt::LeftButton && (m_drag >= 0 || m_holdIdx >= 0)) letGo();
    }

    void leaveEvent(QEvent *evt) override
    {
        if (m_holdIdx >= 0) letGo();
        QWidget::leaveEvent(evt);
    }

private:
    struct State {
        int xpos = 0, ypos = 0;
        unsigned buttons = 0;       /* the buttons the mouse holds */
    };
    struct Seen {
        int xpos = 0, ypos = 0;
        unsigned buttons = 0, dirs = 0;
    };

    QPointF toPixel(double xpos, double ypos) const
    {
        return QPointF(kMargin + xpos * kStickUnit, kMargin + ypos * kStickUnit);
    }
    QRectF rectOf(const msdos_control &ctl) const
    {
        return QRectF(toPixel(ctl.x, ctl.y), QSizeF(ctl.w * kStickUnit, ctl.h * kStickUnit));
    }
    int hitAt(const QPointF &pos) const
    {
        return msdossession_controls_hit(m_mode, (float)((pos.x() - kMargin) / kStickUnit),
                                         (float)((pos.y() - kMargin) / kStickUnit));
    }
    void send(int stick)
    {
        msdossession_pointer_set(m_session, stick, m_state[stick].xpos, m_state[stick].ypos, m_state[stick].buttons);
    }

    /* A drag from the knob's rest centre becomes the stick's position. */
    void steer(const msdos_control &ctl, const QPointF &pos)
    {
        State &state = m_state[ctl.stick];
        msdossession_stick_from_vector((float)((pos.x() - kMargin) / kStickUnit) - ctl.cenx,
                                       (float)((pos.y() - kMargin) / kStickUnit) - ctl.ceny, ctl.reach, &state.xpos,
                                       &state.ypos);
        send(ctl.stick);
        update();
    }

    /* The part's target in Map mode: a button or pill, or the side of the well a
     * click on the plate is nearest. */
    void pickTarget(int idx, const QPointF &pos)
    {
        const msdos_control &ctl = m_ctl[idx];
        if (ctl.target >= 0) {
            m_onPick(ctl.target);
            return;
        }
        if (ctl.kind != MSDOS_CTL_STICK) return;
        const double delx = (pos.x() - kMargin) / kStickUnit - ctl.cenx, dely = (pos.y() - kMargin) / kStickUnit - ctl.ceny;
        int act;
        if (qAbs(delx) > qAbs(dely)) act = delx < 0 ? MSDOS_ACT_LEFT : MSDOS_ACT_RIGHT;
        else act = dely < 0 ? MSDOS_ACT_UP : MSDOS_ACT_DOWN;
        m_onPick(MSDOS_TARGET(ctl.stick, act));
    }

    void hover(const QPointF &pos)
    {
        const int idx = hitAt(pos);
        int target = idx >= 0 ? m_ctl[idx].target : -1;
        if (idx >= 0 && target < 0 && m_ctl[idx].kind == MSDOS_CTL_STICK) target = -2;
        if (target == -2) {
            setToolTip(QStringLiteral("Drag to steer %1").arg(local(m_ctl[idx].label)));
            return;
        }
        if (target < 0) {
            setToolTip(QString());
            return;
        }
        setToolTip(QStringLiteral("%1\nKey: %2    Gamepad: %3")
                       .arg(local(msdos_target_name(target)), orNone(keyName(m_session, target)),
                            orNone(padNames(m_session, target))));
    }

    bool showLegends() const { return m_showBindings || m_map != -2; }

    /* ---- the art ---- */

    void paintCaption(QPainter &pnt, const msdos_control &ctl) const
    {
        const QRectF rect = rectOf(ctl);
        if (local(ctl.label) == QLatin1String("KRAFT")) {
            pnt.setPen(rgbColor(MSDOS_KRAFT_LOGO_RGB));
            pnt.setFont(pixelFont(font(), rect.height() * 0.8, true, true));
            pnt.drawText(rect, Qt::AlignCenter, QStringLiteral("KRAFT"));
            return;
        }
        pnt.setPen(palette().color(QPalette::WindowText));
        pnt.setFont(pixelFont(font(), 13, true));
        pnt.drawText(rect, Qt::AlignCenter, local(ctl.label));
    }

    void paintBody(QPainter &pnt, const msdos_control &ctl) const
    {
        const QRectF rect = rectOf(ctl);
        const QString name = local(ctl.label);
        if (name == QLatin1String("Base")) {
            pnt.setBrush(rgbColor(MSDOS_KRAFT_BODY_RGB));
            pnt.setPen(QPen(rgbColor(MSDOS_KRAFT_RIM_RGB), 4));
            pnt.drawRoundedRect(rect.adjusted(2, 2, -2, -2), 20, 20);
        } else if (name == QLatin1String("Plate")) {
            pnt.setBrush(rgbColor(MSDOS_KRAFT_PLATE_RGB));
            pnt.setPen(QPen(rgbColor(MSDOS_KRAFT_PLATE_RGB).darker(120), 1));
            pnt.drawRoundedRect(rect, 10, 10);
        } else if (name == QLatin1String("Well")) {
            pnt.setBrush(rgbColor(MSDOS_KRAFT_WELL_RGB));
            pnt.setPen(QPen(rgbColor(MSDOS_KRAFT_WELL_RGB).darker(135), 2));
            pnt.drawEllipse(rect);
        } else if (name == QLatin1String("Label")) {
            pnt.setBrush(rgbColor(MSDOS_KRAFT_BODY_RGB).lighter(104));
            pnt.setPen(QPen(rgbColor(MSDOS_KRAFT_RIM_RGB), 1));
            pnt.drawRoundedRect(rect, 3, 3);
        }
    }

    void paintTrim(QPainter &pnt, const msdos_control &ctl) const
    {
        const QRectF rect = rectOf(ctl);
        pnt.setBrush(rgbColor(MSDOS_KRAFT_TRIM_RGB));
        pnt.setPen(QPen(rgbColor(MSDOS_KRAFT_TRIM_RGB).darker(130), 1));
        pnt.drawRoundedRect(rect, 3, 3);
        /* the slider tab at its centre */
        pnt.setBrush(rgbColor(MSDOS_KRAFT_PLATE_RGB).lighter(115));
        const bool vertical = rect.height() > rect.width();
        const QRectF tab = vertical ? QRectF(rect.x() - 1, rect.center().y() - 4, rect.width() + 2, 8)
                                    : QRectF(rect.center().x() - 4, rect.y() - 1, 8, rect.height() + 2);
        pnt.drawRoundedRect(tab, 2, 2);
    }

    /* The stick: crosshair on the well, the shaft to the grip, which follows the
     * merged position (the mouse's or a gamepad's). */
    void paintStick(QPainter &pnt, const msdos_control &ctl) const
    {
        const QPointF centre = toPixel(ctl.cenx, ctl.ceny);
        const QPointF knob = knobPoint(ctl);
        pnt.setPen(QPen(rgbColor(MSDOS_KRAFT_WELL_RGB).darker(125), 1, Qt::DotLine));
        pnt.drawLine(centre - QPointF(2.2 * kStickUnit, 0), centre + QPointF(2.2 * kStickUnit, 0));
        pnt.drawLine(centre - QPointF(0, 2.2 * kStickUnit), centre + QPointF(0, 2.2 * kStickUnit));
        /* the held directions: a red bar on that edge of the well */
        const unsigned dirs = m_seen[ctl.stick].dirs;
        pnt.setPen(Qt::NoPen);
        pnt.setBrush(rgbColor(MSDOS_HELD_RGB));
        const double edge = 2.45 * kStickUnit, span = 1.6 * kStickUnit, thick = 3;
        if (dirs & (1u << MSDOS_ACT_UP)) pnt.drawRoundedRect(QRectF(centre.x() - span / 2, centre.y() - edge, span, thick), 1.5, 1.5);
        if (dirs & (1u << MSDOS_ACT_DOWN)) pnt.drawRoundedRect(QRectF(centre.x() - span / 2, centre.y() + edge - thick, span, thick), 1.5, 1.5);
        if (dirs & (1u << MSDOS_ACT_LEFT)) pnt.drawRoundedRect(QRectF(centre.x() - edge, centre.y() - span / 2, thick, span), 1.5, 1.5);
        if (dirs & (1u << MSDOS_ACT_RIGHT)) pnt.drawRoundedRect(QRectF(centre.x() + edge - thick, centre.y() - span / 2, thick, span), 1.5, 1.5);
        /* shaft, then the moulded grip */
        pnt.setPen(QPen(rgbColor(MSDOS_KRAFT_SHAFT_RGB), 0.55 * kStickUnit, Qt::SolidLine, Qt::RoundCap));
        pnt.drawLine(centre, knob);
        pnt.setPen(QPen(rgbColor(MSDOS_KRAFT_GRIP_RGB).darker(125), 1));
        pnt.setBrush(rgbColor(MSDOS_KRAFT_GRIP_RGB));
        pnt.drawEllipse(knob, 1.15 * kStickUnit, 1.15 * kStickUnit);
        pnt.setPen(QPen(rgbColor(MSDOS_KRAFT_GRIP_RGB).darker(112), 1));
        pnt.setBrush(Qt::NoBrush);
        pnt.drawEllipse(knob, 0.95 * kStickUnit, 0.95 * kStickUnit);
        if (showLegends()) paintDirections(pnt, ctl);
    }

    /* The steering area of a stick (its knob's rest centre and travel). */
    const msdos_control &stickOf(int stick) const
    {
        for (int idx = 0; idx < m_count; ++idx)
            if (m_ctl[idx].kind == MSDOS_CTL_STICK && m_ctl[idx].stick == stick) return m_ctl[idx];
        return m_ctl[0];
    }

    /* Where the grip is now: the merged state of the stick. */
    QPointF knobPoint(const msdos_control &ctl) const
    {
        const QPointF centre = toPixel(ctl.cenx, ctl.ceny);
        const Seen &seen = m_seen[ctl.stick];
        return centre + QPointF(seen.xpos / 32768.0 * ctl.reach * kStickUnit, seen.ypos / 32768.0 * ctl.reach * kStickUnit);
    }

    /* The four sides of the well and what is bound to each. */
    void paintDirections(QPainter &pnt, const msdos_control &ctl) const
    {
        static const char *const glyph[4] = { "▲", "▼", "◀", "▶" };
        const QPointF centre = toPixel(ctl.cenx, ctl.ceny);
        const double reach = 2.0 * kStickUnit;
        const QPointF where[4] = { centre + QPointF(0, -reach), centre + QPointF(0, reach), centre + QPointF(-reach, 0),
                                   centre + QPointF(reach, 0) };
        for (int act = 0; act < 4; ++act) {
            const int target = MSDOS_TARGET(ctl.stick, act);
            const QString key = legendName(keyName(m_session, target));
            const QRectF box(where[act].x() - 0.85 * kStickUnit, where[act].y() - 0.3 * kStickUnit, 1.7 * kStickUnit, 0.6 * kStickUnit);
            const bool picked = m_map == target;
            pnt.setPen(picked ? QPen(msdosAccentColor(), 2) : QPen(rgbColor(MSDOS_KRAFT_WELL_RGB).darker(150), 1, Qt::DashLine));
            pnt.setBrush(picked ? rgbColor(MSDOS_KRAFT_BODY_RGB).lighter(105) : QColor(255, 255, 255, 150));
            pnt.drawRoundedRect(box, 4, 4);
            pnt.setPen(QColor(0x30, 0x30, 0x30));
            drawFitted(pnt, box.adjusted(2, 0, -2, 0), QString::fromUtf8(glyph[act]) + QLatin1Char(' ') + (key.isEmpty() ? QStringLiteral("—") : key),
                       pixelFont(font(), 9.5, true));
        }
    }

    void paintButton(QPainter &pnt, const msdos_control &ctl) const
    {
        const int act = ctl.target % MSDOS_ACT_COUNT;
        const bool held = (m_seen[ctl.stick].buttons >> (act - MSDOS_ACT_B1)) & 1u;
        QPainterPath shape;
        QRectF rect = rectOf(ctl);
        QColor fill = rgbColor(MSDOS_KRAFT_SIDE_RGB);
        if (ctl.kind == MSDOS_CTL_PILL) {
            shape.addRoundedRect(rect, rect.height() / 2, rect.height() / 2);
            fill = rgbColor(MSDOS_KRAFT_CAP_RGB);
        } else if (ctl.reach > 0) {
            /* button 1: the cap on the grip, following the stick */
            const QPointF centre = knobPoint(stickOf(ctl.stick));
            rect = QRectF(centre.x() - ctl.reach * kStickUnit, centre.y() - ctl.reach * kStickUnit, 2 * ctl.reach * kStickUnit,
                          2 * ctl.reach * kStickUnit);
            shape.addEllipse(rect);
            fill = rgbColor(MSDOS_KRAFT_CAP_RGB);
        } else {
            shape.addRoundedRect(rect, 5, 5);
        }
        pnt.setBrush(fill);
        pnt.setPen(QPen(fill.darker(135), 1));
        pnt.drawPath(shape);
        const QString legend = local(ctl.legend);
        pnt.setPen(fill.lightness() < 150 ? QColor(0xF2, 0xF2, 0xEC) : QColor(0x30, 0x30, 0x30));
        const bool showKey = showLegends();
        drawFitted(pnt, rect.adjusted(1, 1, -1, -1), legend, pixelFont(font(), 12, true));
        if (held) paintHeld(pnt, shape);
        if (m_map == ctl.target) {
            pnt.setBrush(Qt::NoBrush);
            pnt.setPen(QPen(msdosAccentColor(), 3));
            pnt.drawPath(shape);
        } else if (m_map != -2) {
            pnt.setBrush(Qt::NoBrush);
            pnt.setPen(QPen(QColor(0x30, 0x30, 0x30, 160), 1, Qt::DashLine));
            pnt.drawPath(shape.translated(0, 0));
        }
        if (showKey) {
            const QString key = legendName(keyName(m_session, ctl.target));
            /* button 1 would hide under the finger: its key goes under the knob */
            QRectF note;
            if (ctl.kind == MSDOS_CTL_PILL) note = QRectF(rect.x() - 6, rect.bottom() + 1, rect.width() + 12, 12);
            else if (ctl.reach > 0) note = QRectF(rect.center().x() - 30, rect.bottom() + 1, 60, 12);
            else note = QRectF(rect.center().x() - 24, rect.bottom() + 2, 48, 12);
            pnt.setPen(QColor(0x30, 0x30, 0x30));
            drawFitted(pnt, note, key.isEmpty() ? QStringLiteral("—") : key, pixelFont(font(), 9.5, true));
        }
    }

    msdossession *m_session;
    std::function<void(int)> m_onPick;
    const msdos_control *m_ctl = nullptr;
    int m_count = 0;
    int m_mode = -1;
    int m_map = -2;
    bool m_showBindings = false;
    int m_drag = -1;                    /* the STICK item being dragged */
    int m_holdIdx = -1;                 /* the BUTTON / PILL item the mouse holds */
    State m_state[MSDOS_JOYSTICKS];
    Seen m_seen[MSDOS_JOYSTICKS];
};

/* ---- ElidedLabel ---------------------------------------------------------------- */

/* A label that never widens the window: it wraps into a fixed height. */
class HintLabel : public QLabel {
public:
    explicit HintLabel(int lines)
    {
        setWordWrap(true);
        setStyleSheet(QStringLiteral("color: gray;"));
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setMinimumWidth(0);
        setFixedHeight(fontMetrics().lineSpacing() * lines + 4);
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
    }
};

/* ---- ControllersWindow ------------------------------------------------------------ */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(msdossession *session, QWidget *parent);
    void present();
    void refresh();
    MsdosKeyHandler onKey;

protected:
    void keyPressEvent(QKeyEvent *evt) override;
    void keyReleaseEvent(QKeyEvent *evt) override;
    void showEvent(QShowEvent *evt) override;
    void hideEvent(QHideEvent *evt) override;
    void changeEvent(QEvent *evt) override;
    /* Tab is the PC's Tab, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, const QString &stolen);
    void pollCapture();
    void pollSlow();
    void sync();
    void rebuildPads();
    void refreshPads();

    msdossession *m_session;
    KeyboardWidget *m_keyboard = nullptr;
    SticksWidget *m_sticks = nullptr;
    QWidget *m_padArea = nullptr;
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "plays Joystick A" */
    unsigned m_padGen = ~0u;
    int m_padPort = -1;                   /* the game port mode the rows were built for */
    QPushButton *m_mapButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    HintLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(msdossession *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel of
     * fixed-size parts and nothing here wants more room. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setProperty("msdosHelpContext", QStringLiteral("controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its parts and kept there: a resizable panel gets tiled and its
     * parts stretched. */
    root->setSizeConstraint(QLayout::SetFixedSize);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    m_keyboard = new KeyboardWidget(session);
    m_keyboard->setObjectName(QStringLiteral("keyboard"));
    root->addWidget(m_keyboard, 0, Qt::AlignHCenter);

    m_sticks = new SticksWidget(session, [this](int target) { setMapState(target); });
    m_sticks->setObjectName(QStringLiteral("sticks"));
    root->addWidget(m_sticks, 0, Qt::AlignHCenter);

    /* Gamepads: one row per pad, rebuilt as they come and go, in room for a
     * fixed number so the window does not change size when one is plugged in */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    auto *padsBox = new QVBoxLayout(pads);
    m_padArea = new QWidget;
    m_padBox = new QVBoxLayout(m_padArea);
    m_padBox->setContentsMargins(0, 0, 0, 0);
    m_padBox->setSpacing(2);
    const int rowHeight = QComboBox().sizeHint().height() + 2;
    m_padArea->setFixedHeight(rowHeight * kMaxPadRows + m_padBox->spacing() * (kMaxPadRows - 1));
    m_padBox->setAlignment(Qt::AlignTop);
    padsBox->addWidget(m_padArea);
    root->addWidget(pads);

    auto *mapRow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    m_mapButton->setToolTip(QStringLiteral("Pick a part, then press a key or a gamepad button to give it"));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    m_clearButton = new QPushButton(QStringLiteral("Clear"));
    m_clearButton->setFocusPolicy(Qt::NoFocus);
    m_clearButton->setToolTip(QStringLiteral("Take the picked control's key and gamepad button away"));
    connect(m_clearButton, &QPushButton::clicked, this, [this] {
        if (m_mapState < 0) return;
        msdossession_binding_clear(m_session, m_mapState);
        m_hint->setText(QStringLiteral("Cleared. Press a key or a gamepad button to bind it again"));
        m_sticks->update();
    });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    defaults->setToolTip(QStringLiteral("Put every key and gamepad button back to its default"));
    connect(defaults, &QPushButton::clicked, this, [this] {
        msdossession_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every control is back to its default."));
        m_sticks->update();
    });
    auto *bindings = new QCheckBox(QStringLiteral("Show bindings"));
    bindings->setFocusPolicy(Qt::NoFocus);
    bindings->setToolTip(QStringLiteral("Show the key each part has (hover for its gamepad button)"));
    bindings->setChecked(msdossession_controls_show_bindings(session) != 0);
    connect(bindings, &QCheckBox::toggled, this, [this](bool checked) {
        msdossession_set_controls_show_bindings(m_session, checked ? 1 : 0);
        m_sticks->setShowBindings(checked);
    });
    m_sticks->setShowBindings(bindings->isChecked());
    mapRow->addWidget(m_mapButton);
    mapRow->addWidget(m_clearButton);
    mapRow->addWidget(defaults);
    mapRow->addSpacing(8);
    mapRow->addWidget(bindings);
    mapRow->addStretch(1);
    mapRow->addWidget(msdosHelpButton(QStringLiteral("controllers")));
    root->addLayout(mapRow);

    m_hint = new HintLabel(2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] {
        m_keyboard->pollHeld();
        m_sticks->pollHeld();
    });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    sync();
    rebuildPads();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    m_keyboard->releaseKey();
    m_sticks->letGo();
}

/* The keyboard follows the machine and the sticks the game port (a change made in Preferences). */
void ControllersWindow::sync()
{
    if (msdossession_machine(m_session) != m_keyboard->machine()) {
        letGo();
        m_keyboard->load();
    }
    m_sticks->syncMode();
}

/* ---- Map mode ------------------------------------------------------------------ */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    m_mapButton->setStyleSheet(state != -2 ? msdosAccentStyle() : QString());
    m_clearButton->setEnabled(state >= 0);
    m_captureTimer.stop();
    msdossession_gamepad_capture_cancel(m_session);
    if (state == -2) {
        m_mapButton->setText(QStringLiteral("Map"));
        m_hint->setText(QStringLiteral("Click a key to press it. Drag a stick's plate to steer it; the buttons are "
                                       "the knob (1), the side pad (2) and the pills (3, 4)."));
    } else {
        /* Map entry lets go of everything: a key held would otherwise stay held under it */
        letGo();
        msdossession_release_all(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        if (state == -1) {
            m_hint->setText(QStringLiteral("Click a part to remap it (a stick: a side of its well). Escape leaves Map."));
        } else {
            m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Escape cancels).")
                                .arg(local(msdos_target_name(state))));
            msdossession_gamepad_capture_begin(m_session);
            m_captureTimer.start(50);
        }
    }
    m_sticks->setMap(state);
    m_sticks->pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const QString &stolen)
{
    setMapState(-1);
    m_hint->setText(stolen.isEmpty() ? QStringLiteral("%1 is now %2").arg(local(msdos_target_name(target)), what)
                                     : QStringLiteral("%1 is now %2 (taken from %3)")
                                           .arg(local(msdos_target_name(target)), what, stolen));
}

void ControllersWindow::pollCapture()
{
    if (m_mapState < 0) {
        m_captureTimer.stop();
        return;
    }
    int button;
    if (!msdossession_gamepad_capture_poll(m_session, &button)) return;
    char stolen[256] = "";
    const int target = m_mapState;
    msdossession_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(msdos_pad_button_name(button)), local(stolen));
}

/* ---- polling ------------------------------------------------------------------------ */

/* Hot-plugged pads, auto-assignment changes, a game port or machine changed in Preferences. */
void ControllersWindow::pollSlow()
{
    sync();
    const unsigned gen = msdossession_gamepad_generation(m_session);
    if (gen != m_padGen || msdossession_game_port_mode(m_session) != m_padPort) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPads();
    }
}

/* ---- gamepads ------------------------------------------------------------------------ */

void ControllersWindow::refreshPads()
{
    for (int idx = 0; idx < (int)m_padEff.size(); ++idx) {
        const int stick = msdossession_gamepad_effective_stick(m_session, idx);
        const QString text = stick < 0 ? QStringLiteral("plays nothing")
                                       : stick == 0 ? QStringLiteral("plays Joystick A") : QStringLiteral("plays Joystick B");
        if (m_padEff[(size_t)idx]->text() != text) m_padEff[(size_t)idx]->setText(text);
    }
}

/* One row per pad: its name, the joystick it plays and the one it is set to. Joystick B is
 * not offered on the 4-button port; with no game port the rows are dimmed. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *row : m_padRows) delete row;
    m_padRows.clear();
    m_padEff.clear();
    const int port = msdossession_game_port_mode(m_session);
    m_padPort = port;
    const int count = msdossession_gamepad_count(m_session);
    if (count == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected - plug one in"));
        none->setStyleSheet(QStringLiteral("color: gray;"));
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    const int sticks = msdossession_joystick_count(m_session);
    for (int idx = 0; idx < count && idx < kMaxPadRows; ++idx) {
        char name[128];
        msdossession_gamepad_name(m_session, idx, name, sizeof name);
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel;
        label->setMinimumWidth(0);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        label->setText(label->fontMetrics().elidedText(local(name), Qt::ElideRight, 360));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(QStringLiteral("color: gray;"));
        /* -1 = automatic, 0 = Joystick A, 1 = Joystick B */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItem(QStringLiteral("Automatic"));
        box->addItem(QStringLiteral("Joystick A"));
        if (sticks >= 2) box->addItem(QStringLiteral("Joystick B"));
        const int assign = msdossession_gamepad_assignment(m_session, idx);
        box->setCurrentIndex(assign < 0 || assign >= sticks ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, idx](int sel) {
            msdossession_gamepad_assign(m_session, idx, sel <= 0 ? -1 : sel - 1);
            refreshPads();
        });
        row->setEnabled(port != MSDOS_PORT_NONE);
        line->addWidget(label, 1);
        line->addWidget(eff);
        line->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    if (count > kMaxPadRows)
        m_padEff.back()->setToolTip(QStringLiteral("%1 more gamepads are connected").arg(count - kMaxPadRows));
    refreshPads();
}

/* ---- the window ----------------------------------------------------------------------- */

void ControllersWindow::present()
{
    m_padGen = msdossession_gamepad_generation(m_session);
    sync();
    rebuildPads();
    show();
    raise();
    activateWindow();
}

void ControllersWindow::refresh()
{
    if (!isVisible()) return;
    sync();
    pollSlow();
    m_keyboard->pollHeld();
    m_sticks->pollHeld();
}

void ControllersWindow::showEvent(QShowEvent *evt)
{
    sync();
    m_keyboard->pollHeld();
    m_sticks->pollHeld();
    m_heldTimer.start(25);
    m_slowTimer.start(250);
    QWidget::showEvent(evt);
}

void ControllersWindow::hideEvent(QHideEvent *evt)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    msdossession_release_all(m_session);
    QWidget::hideEvent(evt);
}

/* The panel losing focus lets go of its mouse and the keyboard sources (the
 * gamepads are left alone). */
void ControllersWindow::changeEvent(QEvent *evt)
{
    if (evt->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        msdossession_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(evt);
}

void ControllersWindow::keyPressEvent(QKeyEvent *evt)
{
    const uint32_t usage = msdosHidFromQt(evt);
    if (m_swallow && usage == m_swallow) return;

    if (m_mapState != -2) {
        if (evt->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (evt->key() == Qt::Key_Escape) {
            m_swallow = usage;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !usage || msdosAppKey(evt)) return;     /* armed, or nothing to bind: nothing is typed */
        char stolen[256] = "";
        const int target = m_mapState;
        msdossession_binding_set_key(m_session, target, usage, stolen, sizeof stolen);
        if (msdossession_binding_get(m_session, target).key != usage) {
            m_hint->setText(QStringLiteral("That key cannot be mapped."));
            return;
        }
        m_swallow = usage;
        bound(target, local(msdossession_hid_name(usage)), local(stolen));
        return;
    }
    if (!onKey || !onKey(evt, true)) QWidget::keyPressEvent(evt);
}

/* A release always goes on, in Map mode too: it only lets go of what a press
 * asserted, so a key held while Map was armed cannot stick. */
void ControllersWindow::keyReleaseEvent(QKeyEvent *evt)
{
    if (evt->isAutoRepeat()) return;
    const uint32_t usage = msdosHidFromQt(evt);
    if (m_swallow && usage == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (!onKey || !onKey(evt, false)) QWidget::keyReleaseEvent(evt);
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
