/* Menu command and resource IDs.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#define IDI_APPICON          101

/* Machine */
#define IDM_OPEN             1001
#define IDM_CAD              1002
#define IDM_RESET            1003
#define IDM_PAUSE            1004
#define IDM_WARP             1005
#define IDM_IMPORT_BIOS      1006
#define IDM_IMPORT_BIOS_DIR  1007
#define IDM_PREFS            1008
#define IDM_EXIT             1009

/* Drives: FujiNet's SD card */
#define IDM_IMPORT_SD        1020
#define IDM_SD_FOLDER        1021
#define IDM_DRIVE_INFO       1022

/* Edit */
#define IDM_COPY_SCREEN      1030
#define IDM_PASTE            1031

/* View */
#define IDM_CONTROLLERS      1040
#define IDM_DEBUGGER         1041
#define IDM_ASPECT           1042
#define IDM_INTEGER          1043
#define IDM_SMOOTH           1044
#define IDM_FULLSCREEN       1045

/* FujiNet, Help */
#define IDM_FUJINET_CONFIG   1050
#define IDM_FUJINET_LOG      1051
#define IDM_FUJINET_RESTART  1052
#define IDM_ABOUT            1060
#define IDM_HELP             1061

/* Ranges: + the drive (A: 0, B: 1; C: 0, D: 1). */
#define IDM_FLOPPY_INSERT    1100
#define IDM_FLOPPY_EJECT     1110
#define IDM_FLOPPY_PROTECT   1120
#define IDM_FLOPPY_BOOT      1130
#define IDM_FLOPPY_BLANK     1140
#define IDM_HD_ATTACH        1150
#define IDM_HD_DETACH        1160
#define IDM_HD_BLANK         1170

/* Controls */
#define IDC_STATUSBAR        2200
#define IDC_CAD_BUTTON       2201
#define IDC_RESET_BUTTON     2202
#define IDC_BIOS_FILES       2203
#define IDC_BIOS_FOLDER      2204
#define IDC_BIOS_HELP        2205
#define IDC_BIOS_ROMS        2206

/* Timers */
#define IDT_STATUS           1
#define IDT_FAST             2
