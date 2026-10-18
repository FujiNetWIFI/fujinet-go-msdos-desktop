/*
 * The main window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>

#include "msdossession.h"

class DisplayWidget;
class QAction;
class QMenu;
class QPushButton;
class QStackedWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(msdossession *session, QWidget *parent = nullptr);
    /* Starts the PC (FujiNet first), or shows the no-BIOS page. */
    void start();
    /* A dropped or command-line file: Open's routing (a floppy goes in A:, a
     * hard disk asks which drive, a BIOS is imported, anything else goes on
     * FujiNet's SD card). */
    void loadMedia(const QString &path);
    /* A key pressed or released with this window, or the Controllers panel,
     * active: the window's own keys (Ctrl+End), else the PC's. True when it
     * was used. */
    bool handleKey(QKeyEvent *e, bool down);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void changeEvent(QEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    bool focusNextPrevChild(bool next) override;

private:
    void buildMenus();
    QWidget *buildNoBiosPage();
    void fillDrivesMenu();
    void updateStatus();
    void updateLights();
    void updateActions();
    void fastTick();
    void showPage();
    void applyView();
    void settingsChanged();
    void showSettings();
    void importBios(bool folder);
    void attachHd(int drive, const QString &path);
    void askHdDrive(const QString &path);
    void togglePref(const char *key, bool on);
    void showAbout();
    void toast(const QString &text);
    void placeToast();
    void toggleControllers();
    QString mediaFolder() const;
    void rememberFolder(const QString &path);

    msdossession *m_session;
    QStackedWidget *m_stack = nullptr;
    DisplayWidget *m_display = nullptr;
    QWidget *m_noBios = nullptr;
    QMenu *m_drives = nullptr;
    QPushButton *m_cadButton = nullptr;
    QPushButton *m_resetButton = nullptr;
    QLabel *m_dot = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_lights[4] = {};
    QLabel *m_toast = nullptr;
    QTimer m_toastTimer;
    QTimer m_statusTimer;
    QTimer m_fastTimer;
    QAction *m_cadAction = nullptr;
    QAction *m_pauseAction = nullptr;
    QAction *m_warpAction = nullptr;
    QAction *m_copyAction = nullptr;
    QAction *m_pasteAction = nullptr;
    QAction *m_aspectAction = nullptr;
    QAction *m_integerAction = nullptr;
    QAction *m_smoothAction = nullptr;
    QAction *m_webUiAction = nullptr;
    QAction *m_restartAction = nullptr;
    unsigned m_padGeneration = 0;
    unsigned m_fujinetGeneration = 0;
    bool m_dotOn = false;
    QString m_shotPath;
    quint64 m_shotFrames = 0, m_quitFrames = 0;
};
