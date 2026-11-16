/*
 * The app's accent as Qt colours, from the session header's macros: the
 * fill, the ink on it, and the accent as text or lines (the light variant on
 * a dark theme, where the fill itself may be too dim).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QPalette>
#include <QWidget>

extern "C" {
#include "msdossession.h"
}

inline QColor msdosDebugRgb(unsigned rgb)
{
    return QColor(int((rgb >> 16) & 0xFF), int((rgb >> 8) & 0xFF), int(rgb & 0xFF));
}

inline QColor msdosDebugAccentColor() { return msdosDebugRgb(MSDOSSESSION_ACCENT_RGB); }
inline QColor msdosDebugAccentInk() { return msdosDebugRgb(MSDOSSESSION_ACCENT_INK_RGB); }

inline bool msdosDebugDarkTheme(const QWidget *widget)
{
    return widget && widget->palette().color(QPalette::Window).lightness() < 128;
}

/* The accent for text and lines: light on a dark theme, the fill otherwise. */
inline QColor msdosDebugAccentTextColor(const QWidget *widget)
{
    return msdosDebugDarkTheme(widget) ? msdosDebugRgb(MSDOSSESSION_ACCENT_LIGHT_RGB) : msdosDebugAccentColor();
}

/* A highlight fill and the ink on it: the badge's silver with black ink on a
 * light theme; on a dark one the light variant, still with black ink. */
inline QColor msdosDebugAccentFill(const QWidget *widget)
{
    return msdosDebugDarkTheme(widget) ? msdosDebugRgb(MSDOSSESSION_ACCENT_LIGHT_RGB) : msdosDebugAccentColor();
}
inline QColor msdosDebugAccentFillInk(const QWidget *widget)
{
    return msdosDebugDarkTheme(widget) ? QColor(Qt::black) : msdosDebugAccentInk();
}
