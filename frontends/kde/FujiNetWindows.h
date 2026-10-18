/*
 * The FujiNet console log window, the FujiNet web UI, and the accent.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

#include "msdossession.h"

/* Shows (raising an existing one) the FujiNet console log window. */
void fujinet_log_show(QWidget *parent, msdossession *session);
/* Opens the FujiNet web UI in the system browser. Not embedded: the web UI's
 * Google and OneDrive Authorize buttons open the provider's consent page in a
 * new tab, and both providers refuse OAuth from an embedded web view. */
void fujinet_config_show(QWidget *parent, msdossession *session);

/* The accent (MSDOSSESSION_ACCENT_RGB, the IBM badge's silver): a fill only,
 * with the badge's ink (#1E1E1E) for text on it. */
QColor msdosAccentColor();
QColor msdosAccentInk();
/* "background: #a8a8a6; color: #1e1e1e;" */
QString msdosAccentStyle();
