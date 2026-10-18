/*
 * FujiNetWindows -- see FujiNetWindows.h.
 *
 * The log is the in-process runtime's captured output, refreshed once a
 * second; it follows the tail only while the reader is at the bottom.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FujiNetWindows.h"

#include <QDesktopServices>
#include <QFont>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

QColor msdosAccentColor()
{
    return QColor((MSDOSSESSION_ACCENT_RGB >> 16) & 0xff, (MSDOSSESSION_ACCENT_RGB >> 8) & 0xff,
                  MSDOSSESSION_ACCENT_RGB & 0xff);
}

QColor msdosAccentInk()
{
    return QColor(0x1e, 0x1e, 0x1e);
}

QString msdosAccentStyle()
{
    return QStringLiteral("background: %1; color: %2;").arg(msdosAccentColor().name(), msdosAccentInk().name());
}

void fujinet_log_show(QWidget *parent, msdossession *session)
{
    static QPointer<QWidget> win;
    if (win) {
        win->show();
        win->raise();
        win->activateWindow();
        return;
    }
    win = new QWidget(parent, Qt::Window);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral("FujiNet Console Log"));
    win->resize(820, 560);

    auto *view = new QPlainTextEdit(win);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    QFont mono = view->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    view->setFont(mono);

    auto *layout = new QVBoxLayout(win);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);

    auto *timer = new QTimer(win);
    auto refresh = [view, session]() {
        static char buf[128 * 1024];
        QScrollBar *bar = view->verticalScrollBar();
        const bool atEnd = bar->value() >= bar->maximum() - 1;
        const int top = bar->value();
        const int n = msdossession_fujinet_copy_log(session, buf, sizeof buf);
        const QString text = n > 0 ? QString::fromUtf8(buf) : QStringLiteral("(no FujiNet output yet)");
        if (text == view->toPlainText())
            return;
        view->setPlainText(text);
        bar->setValue(atEnd ? bar->maximum() : top);
    };
    QObject::connect(timer, &QTimer::timeout, view, refresh);
    timer->start(1000);
    refresh();
    win->show();
}

void fujinet_config_show(QWidget *parent, msdossession *session)
{
    (void)parent;
    QDesktopServices::openUrl(QUrl(QString::fromUtf8(msdossession_fujinet_webui_url(session))));
}
