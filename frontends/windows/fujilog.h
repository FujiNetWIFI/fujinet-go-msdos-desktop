/*
 * The FujiNet console log window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "win_common.h"

/* Shows (raising an existing one) the FujiNet console log window: the
 * in-process runtime's captured output, refreshed once a second. */
void msdos_fujilog_show(HWND parent, msdossession *session);
