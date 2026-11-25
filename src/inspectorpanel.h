/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QDialog>
#include <QPointer>

class QLabel;
class QListWidget;
class QPushButton;
class QTabWidget;
class QTextBrowser;

namespace KateAi
{

class AgentLoop;

/**
 * Read-only Harness inspector.
 *
 * The panel never assembles or compacts the conversation itself: every tab
 * asks AgentLoop for the same report the `:kateai` commands use. The Refresh
 * button and the turn-finished signal only re-read state, so opening the panel
 * cannot change a turn or execute a check.
 */
class InspectorPanel : public QDialog
{
    Q_OBJECT

public:
    explicit InspectorPanel(AgentLoop *agent, QWidget *parent = nullptr);
    ~InspectorPanel() override;

    void refresh();

private:
    void scheduleRefresh();
    void addTextTab(const QString &title, QTextBrowser **view);
    void refreshRewind();

    AgentLoop *m_agent = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTextBrowser *m_context = nullptr;
    QTextBrowser *m_tools = nullptr;
    QTextBrowser *m_memory = nullptr;
    QTextBrowser *m_skills = nullptr;
    QTextBrowser *m_trace = nullptr;
    QTextBrowser *m_cost = nullptr;
    QListWidget *m_checkpoints = nullptr;
    QLabel *m_rewindNote = nullptr;
    QPushButton *m_restore = nullptr;
    bool m_refreshPending = false;
};

} // namespace KateAi
