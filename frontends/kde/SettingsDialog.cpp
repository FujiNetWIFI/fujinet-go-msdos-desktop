/*
 * SettingsDialog -- see SettingsDialog.h.
 *
 * A tab per page of msdossession_pref, a group box per group, a form row per
 * preference, so the four frontends show the same settings. Rows that apply
 * LIVE (the picture, the volume) take effect at once. The machine -- model,
 * processor, memory, cards, drives, ports -- is the computer you would
 * switch off to change: those rows wait, and closing the dialog applies them
 * together with one reset. Rows follow each other as they change:
 * processors and memory by machine, cards by what the machine can take and
 * what has been imported.
 *
 * A ROMs tab lists what each machine and card needs, imported or not.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <vector>

#include "controllers/ControllersWindow.h"

namespace {

struct Row {
    int pref = -1;
    QWidget *widget = nullptr;      /* the editor */
    QWidget *label = nullptr;       /* its form label */
    QString labels;                 /* a choice row's labels, as shown */
};

struct State {
    msdossession *session;
    QWidget *dialog;
    SettingsDialog::Hooks hooks;
    std::vector<Row> rows;
    std::vector<QLabel *> romRows;
    bool syncing = false;
    int pending = MSDOS_APPLY_LIVE;     /* the heaviest change not applied yet */
};

const QString kGray = QStringLiteral("color: gray;");

void refresh(State &st);

/* A change to pref i: LIVE ones apply now unless something heavier waits
 * (applying then would reset mid-edit). */
void changed(State &st, int i)
{
    const msdos_pref *p = msdossession_pref(i);
    if (p->applies > st.pending)
        st.pending = p->applies;
    if (st.pending == MSDOS_APPLY_LIVE)
        msdossession_apply_settings(st.session);
    if (p->applies == MSDOS_APPLY_LIVE && st.hooks.live)
        st.hooks.live();
    if (!qstrcmp(p->key, "machine"))
        msdosControllersRefresh();
    refresh(st);
}

QString choiceLabels(State &st, int i)
{
    QString all;
    const char *l;
    for (int c = 0; (l = msdossession_pref_choice(st.session, i, c)) != nullptr; c++) {
        all += QString::fromUtf8(l);
        if (!msdossession_pref_choice_enabled(st.session, i, c))
            all += QStringLiteral(" (not available)");
        all += QLatin1Char('\n');
    }
    return all;
}

/* Every row from the store: values, labels that depend on others, and
 * which rows apply to this machine. */
void refresh(State &st)
{
    st.syncing = true;
    for (Row &r : st.rows) {
        const msdos_pref *p = msdossession_pref(r.pref);
        const bool on = msdossession_pref_enabled(st.session, r.pref);
        r.widget->setEnabled(on);
        if (r.label)
            r.label->setEnabled(on);
        switch (p->type) {
        case MSDOS_PREF_BOOL:
            static_cast<QCheckBox *>(r.widget)->setChecked(msdossession_pref_get_int(st.session, r.pref) != 0);
            break;
        case MSDOS_PREF_INT: {
            auto *sb = static_cast<QSpinBox *>(r.widget);
            int lo, hi, step;
            msdossession_pref_int_range(st.session, r.pref, &lo, &hi, &step);
            sb->setRange(lo, hi);
            sb->setSingleStep(step > 0 ? step : 1);
            sb->setValue(msdossession_pref_get_int(st.session, r.pref));
            break;
        }
        case MSDOS_PREF_CHOICE: {
            auto *combo = static_cast<QComboBox *>(r.widget);
            const QString labels = choiceLabels(st, r.pref);
            if (labels != r.labels) {
                combo->clear();
                for (const QString &l : labels.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
                    combo->addItem(l);
                r.labels = labels;
            }
            combo->setCurrentIndex(qMax(0, msdossession_pref_get_choice(st.session, r.pref)));
            break;
        }
        default:
            break;
        }
    }
    for (int r = 0; r < (int)st.romRows.size(); r++)
        st.romRows[(size_t)r]->setText(msdossession_rom_item_available(st.session, r)
                                           ? QStringLiteral("Imported") : QStringLiteral("Not imported"));
    st.syncing = false;
}

QWidget *makeEditor(State &st, int i)
{
    const msdos_pref *p = msdossession_pref(i);
    switch (p->type) {
    case MSDOS_PREF_BOOL: {
        auto *cb = new QCheckBox;
        QObject::connect(cb, &QCheckBox::toggled, cb, [&st, i](bool on) {
            if (st.syncing)
                return;
            msdossession_pref_set_int(st.session, i, on);
            changed(st, i);
        });
        return cb;
    }
    case MSDOS_PREF_INT: {
        auto *sb = new QSpinBox;
        sb->setRange(p->min, p->max);
        sb->setSingleStep(p->step > 0 ? p->step : 1);
        sb->setKeyboardTracking(false);
        QObject::connect(sb, &QSpinBox::valueChanged, sb, [&st, i](int v) {
            if (st.syncing)
                return;
            msdossession_pref_set_int(st.session, i, v);
            changed(st, i);
        });
        return sb;
    }
    case MSDOS_PREF_CHOICE: {
        auto *combo = new QComboBox;
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(28);
        QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [&st, i](int c) {
            if (st.syncing || c < 0)
                return;
            if (!msdossession_pref_choice_enabled(st.session, i, c)) {
                /* not imported, or not on this machine: put it back */
                QMessageBox::information(st.dialog, QStringLiteral("Not Available"),
                    QStringLiteral("Not available — import its ROM first, or choose a machine that "
                                   "takes it."));
                refresh(st);
                return;
            }
            msdossession_pref_set_choice(st.session, i, c);
            changed(st, i);
        });
        return combo;
    }
    default:
        return new QLabel;
    }
}

void importRoms(State &st, bool folder)
{
    const QString path = folder
        ? QFileDialog::getExistingDirectory(st.dialog, QStringLiteral("Import BIOS from a Folder"))
        : QFileDialog::getOpenFileName(st.dialog, QStringLiteral("Import BIOS"));
    if (path.isEmpty())
        return;
    char msg[2048];
    const int rc = msdossession_rom_import(st.session, path.toLocal8Bit().constData(), msg, sizeof msg);
    QMessageBox::information(st.dialog, rc == 0 ? QStringLiteral("BIOS Imported") : QStringLiteral("Nothing Imported"),
                             QString::fromUtf8(msg));
    if (st.hooks.imported)
        st.hooks.imported();
    refresh(st);
}

QWidget *romsPage(State &st)
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    QGroupBox *group = nullptr;
    QFormLayout *form = nullptr;
    QString last;
    for (int r = 0; r < msdossession_rom_item_count(); r++) {
        const msdos_rom_item *item = msdossession_rom_item(r);
        const QString g = QString::fromUtf8(item->group);
        if (!group || g != last) {
            group = new QGroupBox(g);
            form = new QFormLayout(group);
            if (last.isEmpty()) {
                auto *note = new QLabel(QStringLiteral(
                    "The machines' BIOS chips and the cards' ROMs are not part of the app. Import them "
                    "from files, a zip or a folder (an 86Box ROM set works); they are recognised by their "
                    "checksums. Kept in %1.").arg(QString::fromUtf8(msdossession_roms_path(st.session))));
                note->setWordWrap(true);
                note->setStyleSheet(kGray);
                auto *buttons = new QHBoxLayout;
                auto *files = new QPushButton(QStringLiteral("Import…"));
                auto *dir = new QPushButton(QStringLiteral("Import a Folder…"));
                QObject::connect(files, &QPushButton::clicked, page, [&st] { importRoms(st, false); });
                QObject::connect(dir, &QPushButton::clicked, page, [&st] { importRoms(st, true); });
                buttons->addWidget(files);
                buttons->addWidget(dir);
                buttons->addStretch(1);
                v->addWidget(note);
                v->addLayout(buttons);
            }
            v->addWidget(group);
            last = g;
        }
        auto *state = new QLabel;
        state->setStyleSheet(kGray);
        auto *name = new QLabel(QString::fromUtf8(item->name));
        form->addRow(name, state);
        st.romRows.push_back(state);
    }
    v->addStretch(1);
    auto *scroll = new QScrollArea;
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

} // namespace

int SettingsDialog::run(QWidget *parent, msdossession *session, const Hooks &hooks)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    dlg.resize(640, 640);
    State st;
    st.session = session;
    st.dialog = &dlg;
    st.hooks = hooks;

    auto *tabs = new QTabWidget;
    QVBoxLayout *pageLayout = nullptr;
    QFormLayout *form = nullptr;
    QString lastPage, lastGroup;
    for (int i = 0; i < msdossession_pref_count(); i++) {
        const msdos_pref *p = msdossession_pref(i);
        const QString pg = QString::fromUtf8(p->page), gr = QString::fromUtf8(p->group);
        if (!pageLayout || pg != lastPage) {
            if (pageLayout)
                pageLayout->addStretch(1);
            auto *page = new QWidget;
            pageLayout = new QVBoxLayout(page);
            auto *scroll = new QScrollArea;
            scroll->setWidget(page);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            tabs->addTab(scroll, pg);
            lastPage = pg;
            lastGroup.clear();
        }
        if (gr != lastGroup) {
            auto *box = new QGroupBox(gr);
            auto *bv = new QVBoxLayout(box);
            if (pg == QLatin1String("Machine") && gr == QLatin1String("Machine")) {
                auto *note = new QLabel(QStringLiteral("The machine's settings apply together when Preferences "
                                                       "closes: the PC resets once."));
                note->setWordWrap(true);
                note->setStyleSheet(kGray);
                bv->addWidget(note);
            }
            form = new QFormLayout;
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            bv->addLayout(form);
            pageLayout->addWidget(box);
            lastGroup = gr;
        }
        Row r;
        r.pref = i;
        r.widget = makeEditor(st, i);
        auto *label = new QLabel(QString::fromUtf8(p->label));
        r.label = label;
        if (p->help) {
            /* the help under the editor, as GNOME's subtitles */
            auto *field = new QWidget;
            auto *fv = new QVBoxLayout(field);
            fv->setContentsMargins(0, 0, 0, 0);
            fv->setSpacing(2);
            auto *help = new QLabel(QString::fromUtf8(p->help));
            help->setWordWrap(true);
            help->setStyleSheet(kGray);
            fv->addWidget(r.widget);
            fv->addWidget(help);
            form->addRow(label, field);
        } else {
            form->addRow(label, r.widget);
        }
        st.rows.push_back(r);
    }
    if (pageLayout)
        pageLayout->addStretch(1);
    tabs->addTab(romsPage(st), QStringLiteral("ROMs"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto *v = new QVBoxLayout(&dlg);
    v->addWidget(tabs);
    v->addWidget(buttons);
    refresh(st);
    dlg.exec();

    msdossession_settings_flush(session);
    return st.pending > MSDOS_APPLY_LIVE ? msdossession_apply_settings(session) : 0;
}
