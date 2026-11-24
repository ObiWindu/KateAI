/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "inspectorpanel.h"

#include "agentloop.h"

#include <KLocalizedString>

#include <QDateTime>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

InspectorPanel::InspectorPanel(AgentLoop *agent, QWidget *parent)
    : QDialog(parent)
    , m_agent(agent)
{
    setWindowTitle(i18n("Kate AI Harness Inspector"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    resize(900, 640);

    auto *layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);

    addTextTab(i18n("Context"), &m_context);
    addTextTab(i18n("Tools"), &m_tools);
    addTextTab(i18n("Rules & Memory"), &m_memory);
    addTextTab(i18n("Hooks & Skills"), &m_skills);
    addTextTab(i18n("Trace"), &m_trace);
    addTextTab(i18n("Cost"), &m_cost);

    // Rewind is deliberately limited to the checkpoint store. Restoring the
    // conversation needs an event-log head operation that does not exist yet;
    // the tab says so instead of pretending.
    auto *rewind = new QWidget(m_tabs);
    auto *rewindLayout = new QVBoxLayout(rewind);
    m_rewindNote = new QLabel(i18n(
        "Checkpoints restore files only. Fork the conversation with :kateai fork. "
        "Conversation rewind needs an event-log head operation and is not part of this release; "
        "the transcript stays as it is."), rewind);
    m_rewindNote->setWordWrap(true);
    m_checkpoints = new QListWidget(rewind);
    m_restore = new QPushButton(i18n("Restore selected checkpoint"), rewind);
    rewindLayout->addWidget(m_rewindNote);
    rewindLayout->addWidget(m_checkpoints, 1);
    rewindLayout->addWidget(m_restore);
    m_tabs->addTab(rewind, i18n("Rewind"));
    connect(m_restore, &QPushButton::clicked, this, [this] {
        const auto *item = m_checkpoints->currentItem();
        if (!item) {
            return;
        }
        const QString id = item->data(Qt::UserRole).toString();
        if (id.isEmpty()) {
            return;
        }
        const auto answer = QMessageBox::question(
            this, i18n("Restore checkpoint"),
            i18n("Restore the workspace files captured by this checkpoint? "
                 "The conversation and the event log are not changed."));
        if (answer != QMessageBox::Yes) {
            return;
        }
        QString error;
        if (!m_agent->restoreCheckpoint(id, &error)) {
            QMessageBox::warning(this, i18n("Restore failed"),
                                 error.isEmpty() ? i18n("The checkpoint could not be restored.") : error);
        } else {
            refresh();
        }
    });

    auto *buttons = new QDialogButtonBox(this);
    auto *refreshButton = buttons->addButton(i18n("Refresh"), QDialogButtonBox::ActionRole);
    auto *closeButton = buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(refreshButton, &QPushButton::clicked, this, &InspectorPanel::refresh);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::hide);

    if (m_agent) {
        connect(m_agent, &AgentLoop::turnFinished, this, [this] {
            QPointer<InspectorPanel> guard(this);
            if (guard) {
                refresh();
            }
        });
        connect(m_agent, &AgentLoop::harnessEvent, this, [this](const QString &, const QJsonObject &) {
            QPointer<InspectorPanel> guard(this);
            if (guard && isVisible()) {
                refresh();
            }
        });
    }

    refresh();
}

InspectorPanel::~InspectorPanel() = default;

void InspectorPanel::addTextTab(const QString &title, QTextBrowser **view)
{
    auto *browser = new QTextBrowser(m_tabs);
    browser->setOpenExternalLinks(false);
    browser->setLineWrapMode(QTextBrowser::WidgetWidth);
    *view = browser;
    m_tabs->addTab(browser, title);
}

void InspectorPanel::refresh()
{
    if (!m_agent) {
        return;
    }
    m_context->setPlainText(m_agent->contextReport() + u"\n\n"_s + m_agent->promptReport());
    m_tools->setPlainText(m_agent->toolsReport());
    m_memory->setPlainText(m_agent->memoryReport());
    m_skills->setPlainText(m_agent->skillsReport() + u"\n\n"_s + m_agent->hooksReport()
                           + u"\n\n"_s + m_agent->subagentsReport());
    m_trace->setPlainText(m_agent->traceReport());
    m_cost->setPlainText(m_agent->usageReport());
    refreshRewind();
}

void InspectorPanel::refreshRewind()
{
    m_checkpoints->clear();
    if (!m_agent) {
        return;
    }
    const QList<CheckpointInfo> checkpoints = m_agent->checkpoints();
    for (const CheckpointInfo &checkpoint : checkpoints) {
        const QString label = checkpoint.label.isEmpty() ? i18n("auto") : checkpoint.label;
        const QString when = checkpoint.createdAt.isValid()
            ? checkpoint.createdAt.toString(Qt::ISODate)
            : i18n("unknown time");
        auto *item = new QListWidgetItem(
            u"%1 — %2 (%3)"_s.arg(label, when, checkpoint.shortId), m_checkpoints);
        item->setData(Qt::UserRole, checkpoint.id);
    }
    m_restore->setEnabled(!checkpoints.isEmpty() && !m_agent->isBusy());
    m_rewindNote->setText(i18n(
        "Checkpoints: %1. Restoring writes the checkpoint's files back and takes a safety "
        "snapshot first. Fork the conversation with :kateai fork; conversation rewind is not "
        "part of this release.",
        checkpoints.size()));
}

} // namespace KateAi
