/*
 * The debugger's Video tab (Qt6 Widgets): a thin view over core/include/msdosvidtab.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QWidget>

#include "msdossession.h"
#include "msdosvidtab.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QScrollArea;
class QTabWidget;
class QTableWidget;

class VideoPicture;
class VideoPalette;

class VideoTab : public QWidget
{
    Q_OBJECT
public:
    explicit VideoTab(msdossession *session, QWidget *parent = nullptr);
    ~VideoTab() override;

    /* Take a snapshot and redraw what changed (force: everything). Cheap to call every tick. */
    void refresh(bool force);

private:
    friend class VideoPicture;
    friend class VideoPalette;
    void refreshView(unsigned what);
    void updatePixelSection();
    void fillRegs();
    void fillPalette();
    void fillMemory();
    void fillTiming();
    void note(const QString &text);

    msdosvid_tab *m_tab = nullptr;
    QComboBox *m_view = nullptr, *m_zoom = nullptr;
    QCheckBox *m_ov[MSDOSVID_O_COUNT] = {};
    QLabel *m_status = nullptr, *m_note = nullptr;
    VideoPicture *m_pic = nullptr;
    QScrollArea *m_scroll = nullptr;
    QTabWidget *m_sections = nullptr;
    QLabel *m_pixelText = nullptr, *m_memText = nullptr, *m_timingText = nullptr;
    QTableWidget *m_regs = nullptr;
    VideoPalette *m_pal = nullptr;
    QLabel *m_palDetail = nullptr;
    QProgressBar *m_beam = nullptr;
    QString m_regsSig;
    bool m_updating = false;
};
