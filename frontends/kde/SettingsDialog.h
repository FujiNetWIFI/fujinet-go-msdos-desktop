/*
 * The Preferences dialog, built from the session's preference table.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QDialog>
#include <functional>

#include "msdossession.h"

namespace SettingsDialog {
struct Hooks {
    std::function<void()> live;         /* a LIVE row changed: the view */
    std::function<void()> imported;     /* ROMs came in: menus, status, page */
};
/* Runs the dialog. Live rows apply at once; the machine's rows apply
 * together when it closes. Returns the msdos_apply that closing needed. */
int run(QWidget *parent, msdossession *session, const Hooks &hooks);
}
