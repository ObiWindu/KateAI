/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "pluginview.h"
#include "workspaceservices.h"
#include "chatwidget.h"
#include "edittracker.h"
#include "editorassist.h"
#include "katedebugbridge.h"
#include "kateecosystembridge.h"
#include "katelspbridge.h"
#include "lsp.h"
#include "mcp.h"
#include "chattheme.h"
#include "configpage.h"
#include "plugin.h"
#include "sessionstore.h"
#include "workspace.h"

#include <KActionCollection>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>
#include <KSyntaxHighlighting/Theme>
#include <KTextEditor/Application>
#include <KTextEditor/Document>
#include <KTextEditor/Editor>
#include <KTextEditor/Message>
#include <KTextEditor/View>
#include <KXMLGUIFactory>

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QKeySequence>
#include <QLayout>
#include <QJsonDocument>
#include <QMainWindow>
#include <QMenu>
#include <QStatusBar>
#include <QToolButton>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

// Reads the branch name from .git/HEAD. This is intentionally a small file
// read instead of `git rev-parse`: editorContext() runs on every debounced
// cursor move, and spawning a process there can freeze the UI for most of a
// second. Handles both a normal repository (.git directory) and a linked
// worktree (.git file containing "gitdir: <path>").
QString gitBranchForWorkspace(const QString &workspace)
{
    if (workspace.isEmpty()) {
        return {};
    }
    const QString gitPath = QDir(workspace).absoluteFilePath(u".git"_s);
    const QFileInfo gitInfo(gitPath);
    QString headPath;
    if (gitInfo.isFile()) {
        QFile gitFile(gitPath);
        if (!gitFile.open(QIODevice::ReadOnly)) {
            return {};
        }
        const QString line = QString::fromUtf8(gitFile.readLine()).trimmed();
        gitFile.close();
        if (!line.startsWith(u"gitdir:"_s)) {
            return {};
        }
        QString gitDir = line.mid(7).trimmed();
        if (QDir::isRelativePath(gitDir)) {
            gitDir = QDir::cleanPath(QDir(workspace).absoluteFilePath(gitDir));
        }
        headPath = QDir(gitDir).absoluteFilePath(u"HEAD"_s);
    } else if (gitInfo.isDir()) {
        headPath = QDir(gitPath).absoluteFilePath(u"HEAD"_s);
    } else {
        return {};
    }

    QFile head(headPath);
    if (!head.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QString ref = QString::fromUtf8(head.readLine()).trimmed();
    head.close();
    const QString prefix = u"ref: refs/heads/"_s;
    if (!ref.startsWith(prefix)) {
        return {}; // detached HEAD: no branch name to report
    }
    return ref.mid(prefix.size());
}

} // namespace

    KateAiView::KateAiView(KateAiPlugin *plugin, KTextEditor::MainWindow *mainWindow)
    : QObject(plugin)
    , m_plugin(plugin)
    , m_mainWindow(mainWindow)
    {
        // Initialize the plugin with its component name and UI resource file
        setComponentName(u"kateai"_s, i18n("Kate AI"));
        setXMLFile(u"ui.rc"_s);
        // Kate restores its session right after the view exists; a workspace
        // change inside this window is Kate's own startup, not a folder the
        // user opened, and is applied without a notice.
        m_viewAge.start();

        // Create the tool view panel on the right side of the main window
        if (m_mainWindow) {
            m_toolView = m_mainWindow->createToolView(plugin,
                                                      u"kateai"_s,
                                                      KTextEditor::MainWindow::Right,
                                                      QIcon::fromTheme(u"help-hint"_s),
                                                      i18n("Kate AI"));
        }

        // Install the configured theme before any widget reads palette colours.
        ChatTheme::loadTheme(plugin->settings());

        // The tool view is the KateAI window: paint it from the window-chrome
        // colours, which follow the editor theme in inherit mode. The chat
        // widget paints its own content on top of it.
        if (m_toolView) {
            m_toolView->setObjectName(u"kateaiToolView"_s);
            m_toolView->setAttribute(Qt::WA_StyledBackground, true);
        }
        applyWindowChrome();

        // Create the chat widget that will contain the AI interface
        if (m_toolView) {
            m_chat = new ChatWidget(m_toolView);
        }

        // The in-editor assist: AI completions at the cursor and hover hints
        // over agent-edited lines. One per window, attached to every editor
        // view of the window. Completions are opt-in through the settings.
        m_assist = new EditorAssist(this);
        m_assist->setSettings(plugin->settings());
        m_assist->setBusyCheck([this]() {
            return m_chat && m_chat->agent() && m_chat->agent()->isBusy();
        });

        // The bridge to Kate's LSP client plugin. It probes the plugin at
        // runtime and reports "not available" when Kate runs without it, so
        // the agent's lsp_* tools and the hover hints degrade gracefully.
        m_lsp = new KateLspBridge(this);
        m_lsp->setMainWindow(m_mainWindow);
        m_assist->setLspProvider(m_lsp);
        connect(m_lsp, &LspProvider::diagnosticsChanged, this, [this](const QString &) {
            // Diagnostic marks changed: the editor context carries their
            // counts, so refresh it (debounced).
            scheduleEditorContextRefresh();
        });

        // Bridge to Kate's DAP/GDB debugger plugin ("kategdbplugin").
        m_debug = new KateDebugBridge(this);
        m_debug->setMainWindow(m_mainWindow);
        m_assist->setDebugProvider(m_debug);
        connect(m_debug, &DebugProvider::stateChanged, this, [this]() {
            scheduleEditorContextRefresh();
            updateStatusBarPill();
        });
        connect(m_debug, &DebugProvider::breakpointsChanged, this, [this]() {
            scheduleEditorContextRefresh();
        });
        connect(m_debug, &DebugProvider::stopped, this, [this](const DebugStopInfo &info) {
            scheduleEditorContextRefresh();
            updateStatusBarPill();
            if (info.isCrashOrException) {
                postDebugStopBanner(info);
            }
        });

        // Bridge to Kate's Build, Konsole, Project Tree, Backtrace, and native tabs.
        m_ecosystem = new KateEcosystemBridge(this);
        m_ecosystem->setMainWindow(m_mainWindow);
        connect(m_ecosystem, &KateEcosystemBridge::projectTreeActionRequested, this,
                [this](const QString &actionId, const QString &path, const QString &baseDir) {
                    if (!m_chat || path.isEmpty()) {
                        return;
                    }
                    showPanel();
                    const QString target = baseDir.isEmpty() ? path : QDir(baseDir).relativeFilePath(path);
                    QString prompt;
                    if (actionId == u"summarize"_s) {
                        prompt = i18n("Summarize the purpose, key symbols, and architecture of `%1`.", target);
                    } else if (actionId == u"tests"_s) {
                        prompt = i18n("Add or improve focused unit tests for `%1`. Explain the proposed coverage first.", target);
                    } else {
                        prompt = i18n("Inspect `%1` and explain how it works and any issues or improvements you notice.", target);
                    }
                    m_chat->ask(prompt);
                });

        // Debounced editor-context refresh on cursor/selection moves: the
        // context string is prompt-sized and the signals fire per keystroke.
        m_contextTimer = new QTimer(this);
        m_contextTimer->setSingleShot(true);
        m_contextTimer->setInterval(300);
        connect(m_contextTimer, &QTimer::timeout, this, [this]() {
            if (m_chat && m_chat->agent()) {
                m_chat->agent()->setEditorContext(editorContext());
            }
        });
        // Debounced project-graph update for the user's own typing.
        m_graphTimer = new QTimer(this);
        m_graphTimer->setSingleShot(true);
        m_graphTimer->setInterval(1500);
        connect(m_graphTimer, &QTimer::timeout, this, [this]() {
            if (m_pendingGraphPath.isEmpty() || !m_chat || !m_chat->agent()) {
                return;
            }
            const QString path = m_pendingGraphPath;
            m_pendingGraphPath.clear();
            // The graph keeps a 4 KiB preview. Reading a huge generated file
            // into the GUI thread only to discard it is wasted memory, so skip
            // the update above the analysis ceiling (ProjectGraph applies the
            // same cap to what it does receive).
            constexpr qint64 kMaxGraphUpdateBytes = 4 * 1024 * 1024;
            const QFileInfo info(path);
            if (info.exists() && info.isFile() && info.size() > kMaxGraphUpdateBytes) {
                return;
            }
            QString content;
            if (m_bridge.readDocument(path, &content)) {
                m_chat->agent()->updateProjectGraph(path, content);
            }
        });

        // Ensure the tool view has a layout and add our chat widget to it
        // Configure the chat widget with plugin settings and set up document bridging
        if (m_toolView && m_chat) {
            if (!m_toolView->layout()) {
                auto *layout = new QVBoxLayout(m_toolView);
                layout->setContentsMargins(0, 0, 0, 0);
            }
            m_toolView->layout()->addWidget(m_chat);
        }

        // Configure the chat widget with plugin settings and set up document bridging
        if (m_chat) {
            m_chat->setSettings(plugin->settings());
            // Edit-in-buffer mode follows the setting, both at startup and
            // on every later change through the config page.
            m_bridge.setEditInBuffer(plugin->settings().editInBuffer);
            if (m_chat->agent()) {
                m_chat->agent()->setDocumentBridge(&m_bridge);
                m_chat->agent()->setLspProvider(m_lsp);
        m_chat->agent()->setDebugProvider(m_debug);
        m_chat->agent()->setEcosystemProvider(m_ecosystem);
                m_chat->agent()->setDebugProvider(m_debug);
                m_chat->agent()->setEcosystemProvider(m_ecosystem);
                // Every chat needs an ID before its first turn. Otherwise the
                // turn-finished autosave is skipped, and switching to another
                // conversation can save the new messages under an empty ID.
                // The session config comes first: it knows which conversation
                // this window was showing when the session was saved, so each
                // window reopens its own chat instead of every window
                // claiming the same global active one. Load first:
                // SessionStore may repair a stale active pointer by falling
                // back to the newest persisted transcript.
                const QString claimedId = plugin->claimSessionConversation();
                SessionStore::SessionData sessionData;
                QString activeId = claimedId;
                if (claimedId.isEmpty()) {
                    sessionData = SessionStore::load();
                    activeId = SessionStore::getActiveConversationId();
                } else {
                    sessionData = SessionStore::loadConversation(claimedId);
                    if (sessionData.loadFailed) {
                        // The stored transcript is unreadable. Restoring it as
                        // an empty chat and autosaving would overwrite the one
                        // copy that might still be salvageable, so start on a
                        // fresh id and leave the damaged row alone.
                        activeId = SessionStore::createNewConversation();
                        sessionData = SessionStore::SessionData();
                        m_chat->showInfoMessage(
                            i18n("The saved conversation could not be read. It was left untouched; starting a new chat so it is not overwritten."),
                            true);
                    } else if (sessionData.messages.isEmpty()
                               && SessionStore::conversationTitle(claimedId).isEmpty()) {
                        // The session pointed at a conversation that no longer
                        // exists (pruned, deleted, or a never-saved new chat).
                        // Leaving the claimed id here opened a blank panel
                        // while perfectly good history sat next to it.
                        sessionData = SessionStore::load();
                        activeId = SessionStore::getActiveConversationId();
                    }
                }
                if (activeId.isEmpty()) {
                    activeId = SessionStore::createNewConversation();
                }
                m_chat->setCurrentConversationId(activeId);

                // Restore session data
                if (!sessionData.messages.isEmpty()) {
                    m_chat->agent()->restoreSession(sessionData);
                    m_chat->rebuildTranscript();
                }
                // The active conversation may be new and not yet listed, but
                // history should still reflect any previously saved threads.
                m_chat->updateHistoryButton();
            }
        }
        // The plugin remembers this view for the session config (its current
        // conversation, per window) and for the ":kateai" command.
        plugin->registerView(this);
        if (m_chat) {
            plugin->noteConversationChanged(this, m_chat->conversationId());
            connect(m_chat, &ChatWidget::conversationChanged, this, [this, plugin](const QString &conversationId) {
                plugin->noteConversationChanged(this, conversationId);
            });
        }
        refreshWorkspace();

        if (plugin && m_mainWindow) {
            connect(plugin, &KateAiPlugin::settingsChanged, this, [this](const Settings &settings) {
                ChatTheme::loadTheme(settings);
                applyWindowChrome();
                if (m_chat) {
                    m_chat->setSettings(settings);
                }
                if (m_assist) {
                    m_assist->setSettings(settings);
                }
                refreshWorkspace();
                updateStatusBarPill();
            });
        }
        // Inheriting the Kate editor theme has to follow the editor live: the
        // window repaints itself when its colour scheme changes, the chat UI
        // inside it does not without this.
        //
        // Two paths can carry the change, and both need handling:
        //  - Editor::configChanged fires when the editor's own configuration
        //    changes, including a manual scheme switch in Kate's settings.
        //  - QEvent::ApplicationPaletteChange (caught via eventFilter on qApp)
        //    fires when KDE switches the system theme (dark <-> light); Kate
        //    follows it and swaps the editor scheme, but configChanged does
        //    not reliably fire for that path.
        //
        // Both can arrive before Editor::theme() reflects the new scheme, so
        // the check is deferred to the next event-loop turn with
        // QTimer::singleShot(0). The comparison uses the theme's background
        // colour rather than only the name: KDE can swap the scheme under the
        // same name, and a name-only check would miss the change.
        // Track the application palette's window colour too: a system theme
        // switch can fire ApplicationPaletteChange without touching the editor
        // scheme, and the tool view must repaint regardless.
        m_lastWindowBg = QGuiApplication::palette().color(QPalette::Window).name();
        if (auto *editor = KTextEditor::Editor::instance()) {
            const auto theme = editor->theme();
            m_lastEditorThemeName = theme.name();
            const QRgb bgRgb = theme.editorColor(KSyntaxHighlighting::Theme::EditorColorRole::BackgroundColor);
            const QRgb styleBgRgb = theme.backgroundColor(KSyntaxHighlighting::Theme::TextStyle::Normal);
            const QColor bg = bgRgb != 0 ? QColor::fromRgb(bgRgb)
                             : styleBgRgb != 0 ? QColor::fromRgb(styleBgRgb)
                             : QColor();
            m_lastEditorThemeBg = bg.isValid() ? bg.name() : QString();
            connect(editor, &KTextEditor::Editor::configChanged, this, [this]() {
                recheckEditorTheme();
            });
        }
        // KDE system theme switch (dark <-> light): Kate follows it and swaps
        // the editor scheme, but configChanged does not reliably fire for this
        // path. ApplicationPaletteChange does.
        qApp->installEventFilter(this);
        if (m_chat) {
            connect(m_chat, &ChatWidget::settingsChanged, plugin, &KateAiPlugin::setSettings);
            connect(m_chat, &ChatWidget::configureRequested, this, &KateAiView::showConfiguration);
            connect(m_chat, &ChatWidget::conversationWorkspaceRequested, this, [this](const QString &workspace) {
                if (workspace.trimmed().isEmpty() || !m_mainWindow) {
                    return;
                }
                const QString folder = QDir::cleanPath(workspace);
                if (!QFileInfo(folder).isDir()) {
                    return;
                }

                // Opening a folder through Kate's document API lets the
                // Projects plugin select the matching project. Apply the
                // workspace explicitly as well, so KateAI follows history
                // even when the project plugin is unavailable.
                m_mainWindow->openUrl(QUrl::fromLocalFile(folder));
                refreshWorkspace(folder);
            });
            connect(m_chat, &ChatWidget::openFileAtLineRequested, this, &KateAiView::openFileAtLine);
            connect(m_chat, &ChatWidget::aboutToSubmit, this, [this]() {
                // A new turn starts: edits the bridge left unsaved in earlier
                // turns are no longer "ours", and the agent must not build on
                // them until they were settled.
                m_bridge.resetBufferedEdits();
                // Completions stay off while the chat turn runs.
                if (m_assist) {
                    m_assist->setAgentBusy(true);
                }
                refreshWorkspace();
                updateStatusBarPill();
            });
            // The turn ends (finishes, fails or is stopped): completions and
            // their budget come back.
            if (m_chat->agent()) {
                connect(m_chat->agent(), &AgentLoop::turnFinished, this, [this]() {
                    if (m_assist) {
                        m_assist->setAgentBusy(false);
                    }
                    updateStatusBarPill();
                });
            }
            // An agent edit of an open document shows where the file lives:
            // the in-editor banner points into the review queue.
            connect(m_chat, &ChatWidget::agentEditedFile, this, [this](const QString &path) {
                postEditBanner(path);
                syncEditRanges();
            });
            // When the queue settles, its banners have nothing left to point
            // at, so the next round may post fresh ones. The hover hints move
            // from "pending review" to "turn finished".
            connect(m_chat, &ChatWidget::pendingEditsChanged, this, [this](bool hasPending) {
                if (!hasPending) {
                    m_bannerPaths.clear();
                }
                syncEditRanges();
                updateStatusBarPill();
            });
        }
        // Buffer mode follows the setting on every later change too.
        connect(plugin, &KateAiPlugin::settingsChanged, this, [this](const Settings &settings) {
            m_bridge.setEditInBuffer(settings.editInBuffer);
        });
        if (m_mainWindow) {
            connect(m_mainWindow, &KTextEditor::MainWindow::viewChanged, this, [this](KTextEditor::View *) {
                refreshWorkspace();
            });
        }
        // Opening a folder does not have to touch any editor view: the
        // active document can stay in the old folder while Kate loads the
        // new one as a project, and switching the active project in the
        // Projects panel (dropdown, Prev/Next, clicking the tree) touches
        // neither the project list nor any editor view. Both have to move
        // the workspace. The project plugin's class is not part of the
        // public KTextEditor API; its signals are, and a Kate without them
        // just leaves the connects unused. Kate restores the session's
        // projects the same way at startup — the view age keeps that first
        // wave of changes quiet.
        connectProjectSignals();
        // Follow the document set: Save-As has to rename the review queue,
        // a close while the agent works needs a notice, and new documents
        // change the completion words.
        if (auto *editor = KTextEditor::Editor::instance()) {
            for (KTextEditor::Document *document : editor->documents()) {
                trackDocument(document);
            }
            connect(editor, &KTextEditor::Editor::documentCreated, this,
                    [this](KTextEditor::Editor *, KTextEditor::Document *document) {
                        trackDocument(document);
                    });
        }

        auto *ac = actionCollection();
        QPointer<QAction> toggle, fresh, ask, fix, refactor, tests, configure, stop, completeAction,
            voicePromptAct, voiceCancelAct;
        QPointer<QAction> diagnoseStopAct, explainStackAct, fixDiagAct, fixBuildAct, launchJsonAct,
            snapshotDebugAct, reviewDiffTabAct, projectGraphTabAct, commitMsgAct, blameAct,
            backtraceAct, termSelAct;
        QPointer<QAction> inspectorAct;

        if (ac) {
            toggle = ac->addAction(u"kateai_toggle"_s);
            if (toggle) {
                toggle->setText(i18n("Show Kate AI"));
                toggle->setIcon(QIcon::fromTheme(u"help-hint"_s));
                KActionCollection::setDefaultShortcut(toggle, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_A));
                connect(toggle, &QAction::triggered, this, &KateAiView::showPanel);
            }

            fresh = ac->addAction(u"kateai_new_chat"_s);
            if (fresh) {
                fresh->setText(i18n("Kate AI: New Chat"));
                KActionCollection::setDefaultShortcut(fresh, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_N));
                connect(fresh, &QAction::triggered, this, &KateAiView::newChat);
            }

            voicePromptAct = ac->addAction(u"kateai_voice_prompt"_s);
            if (voicePromptAct) {
                voicePromptAct->setText(i18n("Kate AI: Voice Prompt (Start / Stop)"));
                voicePromptAct->setIcon(QIcon::fromTheme(u"audio-input-microphone"_s));
                KActionCollection::setDefaultShortcut(voicePromptAct, QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V));
                connect(voicePromptAct, &QAction::triggered, this, &KateAiView::toggleVoicePrompt);
            }

            voiceCancelAct = ac->addAction(u"kateai_voice_cancel"_s);
            if (voiceCancelAct) {
                voiceCancelAct->setText(i18n("Kate AI: Cancel Voice Prompt"));
                voiceCancelAct->setIcon(QIcon::fromTheme(u"dialog-cancel"_s));
                KActionCollection::setDefaultShortcut(
                    voiceCancelAct, QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_V));
                connect(voiceCancelAct, &QAction::triggered, this, &KateAiView::cancelVoicePrompt);
            }

            ask = ac->addAction(u"kateai_ask_selection"_s);
            if (ask) {
                ask->setText(i18n("Ask Kate AI About This"));
                connect(ask, &QAction::triggered, this, &KateAiView::askSelection);
            }

            // Stop has a shortcut of its own: the panel button is at the far
            // end of a scrolled transcript when a turn runs long.
            stop = ac->addAction(u"kateai_stop"_s);
            if (stop) {
                stop->setText(i18n("Stop Kate AI"));
                stop->setIcon(QIcon::fromTheme(u"process-stop"_s));
                connect(stop, &QAction::triggered, this, [this]() {
                    if (m_chat && m_chat->agent()) {
                        m_chat->agent()->abort();
                    }
                });
            }

            fix = ac->addAction(u"kateai_fix_selection"_s);
            if (fix) {
                fix->setText(i18n("Fix Selection"));
                connect(fix, &QAction::triggered, this, [this]() {
                    askSelectionWithInstruction(i18n("Find and fix problems in this code. Explain the change, then apply it."));
                });
            }

            refactor = ac->addAction(u"kateai_refactor_selection"_s);
            if (refactor) {
                refactor->setText(i18n("Refactor Selection"));
                connect(refactor, &QAction::triggered, this, [this]() {
                    askSelectionWithInstruction(i18n("Refactor this code for clarity and maintainability. Explain the change, then apply it."));
                });
            }

            tests = ac->addAction(u"kateai_test_selection"_s);
            if (tests) {
                tests->setText(i18n("Add Tests for Selection"));
                connect(tests, &QAction::triggered, this, [this]() {
                    askSelectionWithInstruction(i18n("Add or improve focused tests for this code. Explain the proposed coverage first."));
                });
            }

            configure = ac->addAction(u"kateai_configure"_s);
            if (configure) {
                configure->setText(i18n("Configure Kate AI…"));
                configure->setIcon(QIcon::fromTheme(u"settings-configure"_s));
                connect(configure, &QAction::triggered, this, &KateAiView::showConfiguration);
            }

            inspectorAct = ac->addAction(u"kateai_inspector"_s);
            if (inspectorAct) {
                inspectorAct->setText(i18n("Open Harness Inspector"));
                inspectorAct->setIcon(QIcon::fromTheme(u"view-statistics"_s));
                connect(inspectorAct, &QAction::triggered, this, &KateAiView::showInspector);
            }

            // Manual completion trigger. The shortcut only reaches the panel's
            // window; the assist is per main window and asks its active view.
            completeAction = ac->addAction(u"kateai_complete"_s);
            if (completeAction) {
                completeAction->setText(i18n("Kate AI: Complete at Cursor"));
                KActionCollection::setDefaultShortcut(
                    completeAction, QKeySequence(Qt::ALT | Qt::Key_Backslash));
                connect(completeAction, &QAction::triggered, this, [this]() {
                    if (m_assist && m_mainWindow) {
                        m_assist->triggerCompletion(m_mainWindow->activeView());
                    }
                });
            }

            diagnoseStopAct = ac->addAction(u"kateai_diagnose_stop"_s);
            if (diagnoseStopAct) {
                diagnoseStopAct->setText(i18n("Diagnose Debugger Stop / Crash"));
                diagnoseStopAct->setIcon(QIcon::fromTheme(u"debug-run"_s));
                connect(diagnoseStopAct, &QAction::triggered, this, &KateAiView::diagnoseDebugStop);
            }

            explainStackAct = ac->addAction(u"kateai_explain_stack"_s);
            if (explainStackAct) {
                explainStackAct->setText(i18n("Explain Call Stack && Locals"));
                connect(explainStackAct, &QAction::triggered, this, &KateAiView::explainDebugStack);
            }

            snapshotDebugAct = ac->addAction(u"kateai_snapshot_debug"_s);
            if (snapshotDebugAct) {
                snapshotDebugAct->setText(i18n("Snapshot / Diff Debugger State"));
                connect(snapshotDebugAct, &QAction::triggered, this, &KateAiView::snapshotDebugState);
            }

            launchJsonAct = ac->addAction(u"kateai_generate_launch_json"_s);
            if (launchJsonAct) {
                launchJsonAct->setText(i18n("Generate .vscode/launch.json for Debugger"));
                connect(launchJsonAct, &QAction::triggered, this, &KateAiView::generateLaunchJson);
            }

            fixDiagAct = ac->addAction(u"kateai_fix_diagnostics"_s);
            if (fixDiagAct) {
                fixDiagAct->setText(i18n("Fix File / Workspace Diagnostics"));
                connect(fixDiagAct, &QAction::triggered, this, &KateAiView::fixDiagnostics);
            }

            fixBuildAct = ac->addAction(u"kateai_fix_build_errors"_s);
            if (fixBuildAct) {
                fixBuildAct->setText(i18n("Fix Build Errors"));
                connect(fixBuildAct, &QAction::triggered, this, &KateAiView::fixBuildErrors);
            }

            backtraceAct = ac->addAction(u"kateai_analyze_backtrace"_s);
            if (backtraceAct) {
                backtraceAct->setText(i18n("Analyze Crash Backtrace"));
                connect(backtraceAct, &QAction::triggered, this, &KateAiView::analyzeBacktrace);
            }

            termSelAct = ac->addAction(u"kateai_explain_terminal"_s);
            if (termSelAct) {
                termSelAct->setText(i18n("Explain Terminal Output"));
                connect(termSelAct, &QAction::triggered, this, &KateAiView::explainTerminalSelection);
            }

            reviewDiffTabAct = ac->addAction(u"kateai_review_diff_tab"_s);
            if (reviewDiffTabAct) {
                reviewDiffTabAct->setText(i18n("Open Pending Edits Diff Tab"));
                connect(reviewDiffTabAct, &QAction::triggered, this, &KateAiView::openReviewDiffTab);
            }

            projectGraphTabAct = ac->addAction(u"kateai_project_graph_tab"_s);
            if (projectGraphTabAct) {
                projectGraphTabAct->setText(i18n("Open Project Graph Tab"));
                connect(projectGraphTabAct, &QAction::triggered, this, &KateAiView::openProjectGraphTab);
            }

            m_annotationAction = ac->addAction(u"kateai_toggle_annotations"_s);
            if (m_annotationAction) {
                m_annotationAction->setText(i18n("Show AI Edit Gutter Annotations"));
                m_annotationAction->setCheckable(true);
                m_annotationAction->setChecked(false);
                connect(m_annotationAction, &QAction::triggered, this, &KateAiView::toggleAnnotationGutter);
            }

            commitMsgAct = ac->addAction(u"kateai_commit_message"_s);
            if (commitMsgAct) {
                commitMsgAct->setText(i18n("Generate Git Commit Message"));
                connect(commitMsgAct, &QAction::triggered, this, &KateAiView::generateCommitMessage);
            }

            blameAct = ac->addAction(u"kateai_explain_git_blame"_s);
            if (blameAct) {
                blameAct->setText(i18n("Explain Line History (Git Blame)"));
                connect(blameAct, &QAction::triggered, this, &KateAiView::explainGitBlame);
            }
        }

        // KTextEditor does not merge plugin XML clients into a view whose context
        // menu was supplied by another plugin. Add this action at show time so it
        // is consistently available in every editor tab.
        if (m_mainWindow) {
            const QList<QAction *> ctxActions = {ask, fix, refactor, tests, fixDiagAct, blameAct, diagnoseStopAct};
            for (auto *view : m_mainWindow->views()) {
                addEditorContextActions(view, ctxActions);
            }
            connect(m_mainWindow, &KTextEditor::MainWindow::viewCreated, this, [this, ctxActions](KTextEditor::View *view) {
                addEditorContextActions(view, ctxActions);
            });
        }

        // The assist follows the window's view set: existing views first, then
        // every view the editor creates later. KTextEditor has no
        // view-about-to-be-destroyed signal; the assist tracks the views with
        // QPointer and drops dead ones on its own.
        if (m_mainWindow && m_assist) {
            for (auto *view : m_mainWindow->views()) {
                m_assist->attachView(view);
                watchViewForContext(view);
            }
            connect(m_mainWindow, &KTextEditor::MainWindow::viewCreated, this,
                    [this](KTextEditor::View *view) {
                        if (m_assist) {
                            m_assist->attachView(view);
                        }
                        watchViewForContext(view);
                    });
            // One-time notice the first time auto mode is about to fire: an
            // idle-triggered completion loop on a metered key is exactly the
            // cost surprise the user should hear about before it happens.
            connect(m_assist, &EditorAssist::autoModeFirstRun, this, [this]() {
                KConfigGroup g(KSharedConfig::openConfig(), u"KateAI"_s);
                if (g.readEntry(u"CompletionAutoNoticeShown"_s, false)) {
                    return;
                }
                g.writeEntry(u"CompletionAutoNoticeShown"_s, true);
                g.sync();
                QVariantMap message;
                message.insert(u"type"_s, u"Information"_s);
                message.insert(u"category"_s, i18n("Kate AI"));
                message.insert(u"categoryIcon"_s, QIcon::fromTheme(u"help-hint"_s));
                message.insert(u"text"_s, i18n("Automatic AI completions are on: Kate AI now requests short completions while you type, within the per-minute budget from its settings. Turn this off any time in the Kate AI configuration."));
                m_mainWindow->showMessage(message);
            });
        }

        if (m_mainWindow && m_mainWindow->guiFactory()) {
            m_mainWindow->guiFactory()->addClient(this);
        }
        setupStatusBarPill();
    }

    KateAiView::~KateAiView()
    {
        // Session is saved in ChatWidget destructor before AgentLoop is destroyed
        // m_toolView is owned by the main window, do not delete it here
        if (m_plugin) {
            m_plugin->unregisterView(this);
        }

        if (m_statusPill) {
            if (m_mainWindow) {
                if (auto *mw = qobject_cast<QMainWindow *>(m_mainWindow->window())) {
                    if (QStatusBar *sb = mw->statusBar()) {
                        sb->removeWidget(m_statusPill);
                    }
                }
            }
            delete m_statusPill.data();
            m_statusPill = nullptr;
        }
        if (m_mainWindow && m_mainWindow->guiFactory()) {
            m_mainWindow->guiFactory()->removeClient(this);
        }
    }

    bool KateAiView::eventFilter(QObject *watched, QEvent *event)
    {
        // QEvent::ApplicationPaletteChange is how Qt 6 delivers a system
        // palette switch (QGuiApplication::paletteChanged is deprecated).
        // KDE dark <-> light lands here; Kate follows by swapping the editor
        // scheme, which the chat panel must mirror when inheriting.
        if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
            recheckEditorTheme();
        }
        return QObject::eventFilter(watched, event);
    }

    void KateAiView::applyWindowChrome()
    {
        // The tool view is the KateAI window: it carries the window-chrome
        // colours, which follow the editor theme in inherit mode. The chat
        // widget paints its own content on top, so this is what keeps the
        // window itself themed wherever it shows through.
        if (!m_toolView) {
            return;
        }
        m_toolView->setStyleSheet(ChatTheme::windowChrome());
    }

    void KateAiView::recheckEditorTheme()
    {
        // configChanged and ApplicationPaletteChange can both fire before
        // Editor::theme() reflects the new scheme. Defer to the next
        // event-loop turn so the theme is settled before we read it.
        QTimer::singleShot(0, this, [this]() {
            if (!m_plugin) {
                return;
            }
            const Settings settings = m_plugin->settings();
            if (settings.themeSource != u"editor"_s || !m_chat) {
                return;
            }
            // In inherit mode both the chat content and the window chrome
            // follow the editor colour scheme, so a scheme change repaints
            // both. The window palette is still tracked: a system theme
            // switch can fire ApplicationPaletteChange without touching the
            // editor scheme, and the tool view must repaint regardless.
            bool changed = QGuiApplication::palette().color(QPalette::Window).name() != m_lastWindowBg;
            m_lastWindowBg = QGuiApplication::palette().color(QPalette::Window).name();
            if (auto *editor = KTextEditor::Editor::instance()) {
                const auto theme = editor->theme();
                if (theme.isValid()) {
                    // Derive the background the same way paletteForEditorTheme does,
                    // so the comparison matches what loadTheme will actually use.
                    const QRgb bgRgb = theme.editorColor(KSyntaxHighlighting::Theme::EditorColorRole::BackgroundColor);
                    const QRgb styleBgRgb = theme.backgroundColor(KSyntaxHighlighting::Theme::TextStyle::Normal);
                    const QColor bg = bgRgb != 0 ? QColor::fromRgb(bgRgb)
                                     : styleBgRgb != 0 ? QColor::fromRgb(styleBgRgb)
                                     : QColor();
                    const QString themeBg = bg.isValid() ? bg.name() : QString();
                    // Compare both the name and the background colour: KDE can swap
                    // the scheme under the same name, and a name-only check would
                    // miss the change.
                    if (theme.name() != m_lastEditorThemeName || themeBg != m_lastEditorThemeBg) {
                        changed = true;
                    }
                    m_lastEditorThemeName = theme.name();
                    m_lastEditorThemeBg = themeBg;
                }
            }
            if (!changed) {
                return;
            }
            // loadTheme installs the new palette into the globals first, so
            // both the tool view and the chat panel read the updated colours.
            ChatTheme::loadTheme(settings);
            applyWindowChrome();
            m_chat->applyTheme();
        });
    }

    void KateAiView::openFileAtLine(const QString &path, int line)
    {
        if (!m_mainWindow || path.isEmpty()) {
            return;
        }
        // openUrl reuses the view already showing the document and opens a new
        // one otherwise, which is what "show me" should mean either way.
        m_mainWindow->openUrl(QUrl::fromLocalFile(path));
        KTextEditor::View *view = m_mainWindow->activeView();
        if (view && line > 0) {
            // Cursors are 0-based; the suggestion line is 1-based.
            view->setCursorPosition(KTextEditor::Cursor(qMax(0, line - 1), 0));
            view->setFocus();
        }
    }

    void KateAiView::showPanel()
    {
        if (m_toolView && m_mainWindow) {
            m_mainWindow->showToolView(m_toolView);
            if (m_chat) {
                m_chat->focusPrompt();
            }
        }
    }

    void KateAiView::showInspector()
    {
        showPanel();
        if (m_chat) {
            m_chat->showInspector();
        }
    }

    void KateAiView::newChat()
    {
        showPanel();
        if (m_chat) {
            m_chat->newChat();
        }
    }

    void KateAiView::toggleVoicePrompt()
    {
        showPanel();
        if (m_chat) {
            m_chat->toggleVoicePrompt();
        }
    }

    void KateAiView::cancelVoicePrompt()
    {
        if (m_chat) {
            m_chat->cancelVoicePrompt();
        }
    }

    void KateAiView::askSelection()
    {
        if (m_mainWindow) {
            askSelectionWithInstruction(i18n("Explain this code."));
        }
    }

    void KateAiView::askSelectionWithInstruction(const QString &instruction)
    {
        // Show the AI panel and get the currently active editor view
        showPanel();
        if (!m_mainWindow) {
            return;
        }
        auto *view = m_mainWindow->activeView();
        if (!view) {
            return;
        }

        // Extract the selected text or the current line if nothing is
        // selected. With several selections (multi-cursor), every range goes
        // in with its line numbers, so the model can point edits at each.
        QString snippet;
        const QList<KTextEditor::Range> ranges = view->selectionRanges();
        if (ranges.size() > 1) {
            QStringList parts;
            for (const KTextEditor::Range &range : ranges) {
                if (range.isEmpty()) {
                    continue;
                }
                parts.append(u"Lines %1-%2:\n%3"_s
                                 .arg(range.start().line() + 1)
                                 .arg(range.end().line() + 1)
                                 .arg(view->document()->text(range)));
            }
            snippet = parts.join(u"\n\n"_s);
        }
        if (snippet.isEmpty()) {
            snippet = view->selectionText();
        }
        if (snippet.isEmpty()) {
            snippet = view->document()->line(view->cursorPosition().line());
        }

        // Get the file path or document name for context
        const QString path = view->document()->url().toLocalFile();

        // Create a comprehensive prompt with instruction, file info, and code snippet
        const QString prompt = i18n("%1\n\nFile: %2\n\nCode:\n%3",
                                    instruction,
                                    path.isEmpty() ? view->document()->documentName() : path,
                                    snippet);

        // Send the prompt to the AI for analysis
        if (m_chat) {
            m_chat->ask(prompt);
        }
    }

    void KateAiView::showConfiguration()
    {
        if (!m_mainWindow) {
            return;
        }
        // If configuration dialog already exists, bring it to the front
        if (m_configDialog) {
            m_configDialog->raise();
            m_configDialog->activateWindow();
            return;
        }

        // Create a new configuration dialog as a child of the main window
        auto *dialog = new QDialog(m_mainWindow->window());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(i18n("Configure Kate AI"));
        dialog->resize(760, 620);

        // Set up the dialog layout with configuration page and buttons
        auto *layout = new QVBoxLayout(dialog);
        auto *page = new KateAiConfigPage(dialog, m_plugin);
        // MCP servers are stored per workspace, so tell the page which one.
        page->setWorkspace(detectWorkspace(m_mainWindow));
        layout->addWidget(page);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, dialog);
        layout->addWidget(buttons);

        // Connect dialog buttons to their respective actions
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, page, &KateAiConfigPage::apply);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        connect(dialog, &QObject::destroyed, this, [this]() {
            m_configDialog = nullptr;
        });

        // Store reference to the dialog and show it
        m_configDialog = dialog;
        dialog->show();
    }

    void KateAiView::addEditorContextActions(KTextEditor::View *view, const QList<QAction *> &actions)
    {
        if (!view) {
            return;
        }
        connect(view, &KTextEditor::View::contextMenuAboutToShow, this, [actions](KTextEditor::View *, QMenu *menu) {
            if (!menu) {
                return;
            }
            // The actions live in a submenu, so checking menu->actions().contains(action)
            // always fails. Detect an existing Kate AI submenu instead (by objectName
            // or title) to avoid stacking duplicates on every contextMenuAboutToShow.
            static const QString menuObjectName = QStringLiteral("kateai_context_menu");
            const QString menuTitle = i18n("Kate AI");
            for (QAction *a : menu->actions()) {
                if (QMenu *sub = a->menu()) {
                    if (sub->objectName() == menuObjectName || a->text() == menuTitle) {
                        return;
                    }
                }
            }
            auto *aiMenu = menu->addMenu(menuTitle);
            aiMenu->setObjectName(menuObjectName);
            for (QAction *action : actions) {
                aiMenu->addAction(action);
            }
        });
    }

    QString KateAiView::editorContext() const
    {
        QStringList lines;
        auto *view = m_mainWindow->activeView();
        if (view && m_mainWindow) {
            const QString path = view->document()->url().toLocalFile();
            if (path.isEmpty()) {
                // An untitled buffer has no path the tools could name, so
                // its content is the only way the model can see it at all.
                lines.append(u"Active file: (untitled) %1"_s.arg(view->document()->documentName()));
                const QString text = view->document()->text();
                if (!text.isEmpty()) {
                    lines.append(u"Unsaved buffer content:\n%1"_s.arg(text.left(2000)));
                }
            } else {
                lines.append(u"Active file: %1"_s.arg(path));
            }
            const KTextEditor::Cursor cursor = view->cursorPosition();
            lines.append(u"Cursor: line %1, column %2"_s.arg(cursor.line() + 1).arg(cursor.column() + 1));
            if (view->document()->isModified()) {
                lines.append(u"Active file has unsaved changes."_s);
            }
            const QString mode = view->document()->highlightingMode();
            if (!mode.isEmpty() && mode != u"None"_s) {
                lines.append(u"Language: %1"_s.arg(mode));
            }
            // LSP surface: whether Kate's LSP client plugin is loaded, and the
            // diagnostic counts for the active file when it has any.
            if (m_lsp) {
                lines.append(m_lsp->isAvailable() ? u"LSP: plugin loaded"_s : u"LSP: not available"_s);
                if (m_lsp->isAvailable() && !path.isEmpty()) {
                    const QList<LspDiagnostic> diags = m_lsp->diagnostics(path);
                    int errs = 0;
                    int warns = 0;
                    for (const LspDiagnostic &d : diags) {
                        if (d.severity == 1) {
                            ++errs;
                        } else if (d.severity == 2) {
                            ++warns;
                        }
                    }
                    if (errs > 0 || warns > 0) {
                        lines.append(u"Diagnostics in active file: %1 error(s), %2 warning(s)"_s.arg(errs).arg(warns));
                    }
                }
            }
            // The symbol the cursor sits in, from the project graph, so the
            // model knows the local context without reading the file first.
            if (!path.isEmpty() && m_chat && m_chat->agent() && m_chat->agent()->getProjectGraph()) {
                const GraphSymbol sym = m_chat->agent()->getProjectGraph()->enclosingSymbol(path, cursor.line() + 1);
                if (!sym.name.isEmpty()) {
                    lines.append(u"Enclosing symbol: %1 %2 (line %3)"_s.arg(sym.kind, sym.name).arg(sym.line));
                }
            }
            if (view->selection()) {
                // The range, not only the text: "where" is what the model
                // needs to point an edit at the same region.
                const KTextEditor::Range range = view->selectionRange();
                lines.append(u"Selection: lines %1-%2\n%3"_s
                                 .arg(range.start().line() + 1)
                                 .arg(range.end().line() + 1)
                                 .arg(view->selectionText().left(4000)));
            }
        }
        QStringList openFiles;
        QStringList bookmarks;
        for (auto *v : m_mainWindow ? m_mainWindow->views() : QList<KTextEditor::View *>()) {
            if (!v || !v->document()) {
                continue;
            }
            const QString path = v->document()->url().toLocalFile();
            const QString label = path.isEmpty()
                ? u"(untitled) %1"_s.arg(v->document()->documentName())
                : path;
            // The modified flag matters to the model: writing a modified
            // document is refused, so it should save or warn first.
            openFiles.append(v->document()->isModified() ? u"%1 [unsaved]"_s.arg(label) : label);
            // Bookmarks are the user's own "interesting lines" markers; the
            // agent should treat them with the same importance. KF6 exposes
            // marks directly on Document, no interface cast needed.
            const auto docMarks = v->document()->marks();
            for (auto markIt = docMarks.cbegin(); markIt != docMarks.cend(); ++markIt) {
                if (markIt.value()->type & KTextEditor::Document::Bookmark) {
                    bookmarks.append(u"%1:%2"_s.arg(label).arg(markIt.key() + 1));
                }
            }
        }
        // The branch the workspace is on: cheap, and it tells the model which
        // line of work the edits belong to. Read HEAD directly rather than
        // spawning git on a path that runs for every cursor pause.
        if (!m_workspace.isEmpty()) {
            const QString branch = gitBranchForWorkspace(m_workspace);
            if (!branch.isEmpty()) {
                lines.append(u"Git branch: %1"_s.arg(branch));
            }
        }
        if (!openFiles.isEmpty()) {
            lines.append(u"Open files:\n- %1"_s.arg(openFiles.join(u"\n- "_s)));
        }
        if (!bookmarks.isEmpty()) {
            // The list can grow with every open file; a compact cap keeps the
            // context budget for the files themselves.
            lines.append(u"Bookmarks:\n- %1"_s.arg(bookmarks.join(u"\n- "_s).left(1000)));
        }
        if (m_debug && m_debug->isAvailable()) {
            const QString dbgSummary = m_debug->contextSummary(8, 24, 1000);
            if (!dbgSummary.isEmpty()) {
                lines.append(dbgSummary);
            }
        }
        return lines.join(u'\n');
    }

    void KateAiView::confirmWorkspaceMcpServers(const QString &workspace)
    {
        if (workspace.isEmpty() || !m_chat || !m_chat->agent()) {
            return;
        }
        McpManager *manager = m_chat->agent()->mcpManager();
        if (!manager) {
            return;
        }
        // Another window on the same workspace already confirmed the list. The
        // manager is shared, so its answer applies here too.
        if (manager->loadsWorkspaceConfig()) {
            return;
        }
        // The answer is remembered for the session: refreshWorkspace() runs on
        // every editor change, and the question would pop up again and again.
        const auto known = m_mcpTrust.constFind(workspace);
        if (known != m_mcpTrust.constEnd()) {
            manager->setLoadWorkspaceConfig(known.value());
            if (known.value()) {
                manager->reloadFromDisk();
            }
            return;
        }

        const QList<McpServerConfig> servers = McpConfigStore::load(workspace);
        if (servers.isEmpty()) {
            m_mcpTrust.insert(workspace, true);
            manager->setLoadWorkspaceConfig(true);
            manager->reloadFromDisk();
            return;
        }

        // A project can ship a server list, and a server is a program that
        // runs with the user's privileges. The list is only used after the
        // user has seen it and agreed to it.
        QStringList names;
        QStringList commands;
        for (const McpServerConfig &server : servers) {
            names.append(server.name);
            commands.append(u"%1: %2"_s.arg(server.name, server.commandLine()));
        }
        QString message = i18n(
            "The project %1 defines %n MCP server(s): %2.\n\n"
            "Starting a server runs a program on your machine. Only continue if you trust this project.",
            names.size(),
            names.join(u", "_s));
        // The names come from the project, so they say nothing about what runs.
        // The command line is what the user has to judge, so it is in the
        // message itself and not only in the collapsible details.
        message += u"\n\n"_s + i18n("These commands will run:") + u"\n"_s + commands.join(u'\n');
        QMessageBox box(QMessageBox::Question, i18n("MCP servers in this project"), message,
                        QMessageBox::Yes | QMessageBox::No, qobject_cast<QWidget *>(m_mainWindow));
        box.setDefaultButton(QMessageBox::No);
        box.setDetailedText(McpConfigStore::configPath(workspace));
        const bool trusted = box.exec() == QMessageBox::Yes;
        m_mcpTrust.insert(workspace, trusted);
        manager->setLoadWorkspaceConfig(trusted);
        if (trusted) {
            manager->reloadFromDisk();
        } else {
            // Nothing of the file is started, and the servers of the previous
            // workspace are stopped as well.
            manager->setServers({});
            manager->disconnectAll();
        }
    }

    void KateAiView::connectProjectSignals()
    {
        // Plugin add/remove: a folder was opened or closed.
        if (auto *projects = projectPlugin()) {
            connect(projects, SIGNAL(projectAdded(QObject *)), this, SLOT(projectListChanged()));
            connect(projects, SIGNAL(projectRemoved(QObject *)), this, SLOT(projectListChanged()));
            // projectCreated is the canonical signal; projectAdded is only
            // forwarded to it, so listen to both. The type name must match
            // the Kate header (KateProject*) for the string connect to bind.
            connect(projects, SIGNAL(projectCreated(KateProject *)), this, SLOT(projectListChanged()));
        }
        // Panel activation: the user switched which open folder is current
        // (dropdown, Prev/Next, clicking the tree). Emitted by the
        // per-window plugin view, not the plugin.
        if (m_mainWindow) {
            if (QObject *panel = m_mainWindow->pluginView(u"kateprojectplugin"_s)) {
                connect(panel, SIGNAL(pluginProjectAdded(QString, QString)), this, SLOT(projectOpened(QString, QString)));
                connect(panel, SIGNAL(pluginProjectRemoved(QString, QString)), this, SLOT(projectListChanged()));
                connect(panel, SIGNAL(projectMapChanged()), this, SLOT(projectActiveChanged()));
                connect(panel, SIGNAL(projectFileNameChanged()), this, SLOT(projectActiveChanged()));
                connect(panel, SIGNAL(projectFilesChanged()), this, SLOT(projectActiveChanged()));
            }
            // The project plugin (or its view) can load after this view:
            // hook up and re-evaluate as soon as it appears.
            connect(m_mainWindow, &KTextEditor::MainWindow::pluginViewCreated, this,
                    [this](const QString &name, QObject *pluginView) {
                        if (name == u"kateprojectplugin"_s) {
                            connect(pluginView, SIGNAL(pluginProjectAdded(QString, QString)), this, SLOT(projectOpened(QString, QString)));
                            connect(pluginView, SIGNAL(pluginProjectRemoved(QString, QString)), this, SLOT(projectListChanged()));
                            connect(pluginView, SIGNAL(projectMapChanged()), this, SLOT(projectActiveChanged()));
                            connect(pluginView, SIGNAL(projectFileNameChanged()), this, SLOT(projectActiveChanged()));
                            connect(pluginView, SIGNAL(projectFilesChanged()), this, SLOT(projectActiveChanged()));
                            projectActiveChanged();
                        }
                    },
                    Qt::UniqueConnection);
        }
        // Same late-load race at application level: the plugin object itself
        // may not exist yet when this view is created. Only react to the
        // project plugin; any other plugin load leaves the folder alone.
        if (auto *editor = KTextEditor::Editor::instance()) {
            if (auto *app = editor->application()) {
                QObject::connect(app, &KTextEditor::Application::pluginCreated, this,
                                 [this](const QString &name, KTextEditor::Plugin *) {
                                     onPluginCreated(name);
                                 });
            }
        }
    }

    void KateAiView::onPluginCreated(const QString &name)
    {
        if (name != u"kateprojectplugin"_s) {
            return;
        }
        // The plugin arrived late: (re)hook its signals, then follow the
        // folder it brings.
        connectProjectSignals();
        projectListChanged();
    }

    void KateAiView::projectListChanged()
    {
        // A folder was opened or closed. detectWorkspace() prefers the
        // active document's project, which still points at the previous
        // folder when the user opens a new one without switching documents.
        // The panel's active folder (falling back to the newest project) is
        // the folder the user just opened, so follow it directly instead of
        // letting the active document veto the switch.
        //
        // When a folder is reopened (it is already in the project list) Kate
        // may not move the panel's active selection synchronously, and the
        // newest project is unchanged, so neither candidate differs from the
        // current workspace on its own. Prefer whichever candidate actually
        // differs from the current workspace, so reopening a folder or going
        // back to one opened earlier still switches instead of being ignored.
        const QString active = activeProjectBaseDir(m_mainWindow);
        const QString newest = newestProjectBaseDir();
        const QString current = m_workspace;
        QString chosen;
        if (!active.isEmpty() && QDir::cleanPath(active) != QDir::cleanPath(current)) {
            chosen = active;
        } else if (!newest.isEmpty() && QDir::cleanPath(newest) != QDir::cleanPath(current)) {
            chosen = newest;
        } else if (!active.isEmpty()) {
            chosen = active;
        } else if (!newest.isEmpty()) {
            chosen = newest;
        }
        if (!chosen.isEmpty()) {
            refreshWorkspace(chosen);
        } else {
            refreshWorkspace();
        }
    }

    void KateAiView::projectOpened(const QString &first, const QString &second)
    {
        // The panel announces a project by name and base directory, in an
        // order that is not guaranteed across Kate versions. The argument
        // that names an existing directory is the folder to switch to.
        QString dir;
        if (QFileInfo(first).isDir()) {
            dir = first;
        } else if (QFileInfo(second).isDir()) {
            dir = second;
        }
        if (!dir.isEmpty()) {
            refreshWorkspace(QDir::cleanPath(dir));
        } else {
            projectActiveChanged();
        }
    }

    void KateAiView::projectActiveChanged()
    {
        // Same explicit override as projectListChanged(): the panel's active
        // folder wins over the still-open document from the previous folder.
        // Without the override detectWorkspace() would keep returning the
        // old document's project and the switch would never be seen.
        const QString active = activeProjectBaseDir(m_mainWindow);
        const QString newest = newestProjectBaseDir();
        const QString current = m_workspace;
        QString chosen;
        if (!active.isEmpty() && QDir::cleanPath(active) != QDir::cleanPath(current)) {
            chosen = active;
        } else if (!newest.isEmpty() && QDir::cleanPath(newest) != QDir::cleanPath(current)) {
            chosen = newest;
        } else if (!active.isEmpty()) {
            chosen = active;
        } else if (!newest.isEmpty()) {
            chosen = newest;
        }
        if (!chosen.isEmpty()) {
            refreshWorkspace(chosen);
        } else {
            refreshWorkspace();
        }
    }

    void KateAiView::refreshWorkspace(const QString &explicitWorkspace)
    {
        if (!m_mainWindow || !m_chat || !m_chat->agent()) {
            return;
        }
        const QString workspace = explicitWorkspace.isEmpty()
            ? detectWorkspace(m_mainWindow)
            : explicitWorkspace;
        // A real workspace switch (Kate opened another folder, a file from
        // outside the project became active) has to be seen where the user
        // types: agent context, checkpoints, MCP servers and completions all
        // follow the new folder. The first detection settles silently, and so
        // does a switch inside the startup window — that is Kate restoring
        // its session, not the user opening a folder.
        if (m_workspace.isEmpty()) {
            m_workspace = workspace;
        } else if (QDir::cleanPath(workspace) != QDir::cleanPath(m_workspace)) {
            const bool announce = m_viewAge.elapsed() > 10000;
            m_workspace = workspace;
            if (announce) {
                m_chat->showWorkspaceNotice(workspace);
            }
        }
        // Share the expensive harness state with every other window on this
        // workspace. The services are installed before setWorkspace(), so the
        // loop adopts the already-built graph instead of indexing the project
        // a second time.
        if (m_plugin && !workspace.isEmpty() && !m_chat->agent()->isBusy()) {
            if (auto services = m_plugin->workspaceServices(workspace)) {
                m_workspaceServices = services;
                m_chat->agent()->setHarnessServices(services->graph(),
                                                    services->mcpManager(),
                                                    services->eventStore());
            }
        }
        confirmWorkspaceMcpServers(workspace);
        m_chat->agent()->setWorkspace(workspace);
        m_chat->agent()->setDocumentBridge(&m_bridge);
        m_chat->agent()->setLspProvider(m_lsp);
        m_chat->agent()->setEditorContext(editorContext());
        updateCompletions();
    }

    void KateAiView::scheduleEditorContextRefresh()
    {
        if (m_contextTimer) {
            m_contextTimer->start();
        }
    }

    void KateAiView::watchViewForContext(KTextEditor::View *view)
    {
        if (!view) {
            return;
        }
        connect(view, &KTextEditor::View::cursorPositionChanged, this, [this](KTextEditor::View *, const KTextEditor::Cursor &) {
            scheduleEditorContextRefresh();
        });
        connect(view, &KTextEditor::View::selectionChanged, this, [this](KTextEditor::View *) {
            scheduleEditorContextRefresh();
        });
    }

    void KateAiView::scheduleGraphUpdate(const QString &path, const QString &content)
    {
        Q_UNUSED(content); // re-read from the buffer at fire time
        m_pendingGraphPath = path;
        if (m_graphTimer) {
            m_graphTimer->start();
        }
    }

    void KateAiView::updateCompletions()
    {
        if (!m_chat || !m_mainWindow) {
            return;
        }
        QStringList words = {u"active"_s, u"selection"_s, u"workspace"_s};
        const QString workspace = detectWorkspace(m_mainWindow);

        for (auto *view : m_mainWindow->views()) {
            if (!view || !view->document()) {
                continue;
            }
            const QString fullPath = view->document()->url().toLocalFile();
            const QString docName = view->document()->documentName();
            if (!docName.isEmpty() && !words.contains(docName)) {
                words.append(docName);
            }
            if (!fullPath.isEmpty() && !workspace.isEmpty() && fullPath.startsWith(workspace)) {
                QString relPath = fullPath.mid(workspace.length());
                if (relPath.startsWith(u'/')) {
                    relPath = relPath.mid(1);
                }
                if (!relPath.isEmpty() && !words.contains(relPath)) {
                    words.append(relPath);
                }
            }
        }
        m_chat->setCompletionWords(words);
    }

    bool KateAiView::ownsEditorView(KTextEditor::View *view) const
    {
        if (!m_mainWindow || !view) {
            return false;
        }
        return m_mainWindow->views().contains(view);
    }

    void KateAiView::trackDocument(KTextEditor::Document *document)
    {
        if (!document || m_docPaths.contains(document)) {
            return;
        }
        m_docPaths.insert(document, document->url().toLocalFile());
        // The user's own typing updates the project graph too, not only the
        // agent's edits: debounced, and the content is re-read from the buffer
        // when the timer fires so the graph gets the latest text.
        connect(document, &KTextEditor::Document::textChanged, this, [this, document]() {
            const QString path = m_docPaths.value(document);
            if (!path.isEmpty()) {
                scheduleGraphUpdate(path, QString());
            }
        });
        // Save-As: the review queue and every in-flight edit of the turn have
        // to follow the file to its new path, or a later accept or reject
        // would settle a path that no longer exists.
        connect(document, &KTextEditor::Document::documentUrlChanged, this,
                [this, document](KTextEditor::Document *) {
                    const QString oldPath = m_docPaths.value(document);
                    const QString newPath = document->url().toLocalFile();
                    m_docPaths[document] = newPath;
                    if (!oldPath.isEmpty() && !newPath.isEmpty() && oldPath != newPath && m_chat) {
                        m_chat->renamePendingEdits(oldPath, newPath);
                    }
                    updateCompletions();
                });
        // A close while the agent is working on the file means the next
        // write_file would recreate it on disk, out of the editor. The
        // message view is where a background event like this belongs.
        connect(document, &KTextEditor::Document::aboutToClose, this, [this, document]() {
            const QString path = m_docPaths.take(document);
            m_bannerPaths.remove(path);
            if (!m_chat || !m_chat->agent() || path.isEmpty()) {
                return;
            }
            if (m_chat->agent()->isBusy() && m_mainWindow) {
                QVariantMap message;
                message.insert(u"type"_s, u"Warning"_s);
                message.insert(u"category"_s, i18n("Kate AI"));
                message.insert(u"categoryIcon"_s, QIcon::fromTheme(u"help-hint"_s));
                message.insert(u"text"_s, i18n("%1 was closed while the agent is working. Further edits to it are written to disk and reopen outside the editor.", QDir::toNativeSeparators(path)));
                m_mainWindow->showMessage(message);
            }
        });
        // The deletion of a document that was never closed still has to
        // drop the entry, or the hash would hold a dangling key. destroyed
        // fires while the pointer value is still meaningful, which is all
        // the hash removal needs.
        connect(document, &QObject::destroyed, this, [this, document]() {
            m_docPaths.remove(document);
        });
        updateCompletions();
    }

    void KateAiView::syncEditRanges()
    {
        if (!m_assist || !m_chat || !m_chat->editTracker()) {
            return;
        }
        EditTracker *tracker = m_chat->editTracker();
        for (const QString &path : tracker->rangedPaths()) {
            m_assist->updateEditRanges(path, tracker->pendingEditRanges(path),
                                       tracker->settledEditRanges(path));
        }
    }

    void KateAiView::postEditBanner(const QString &path)
    {
        if (path.isEmpty() || !m_plugin || !m_plugin->settings().editBannersEnabled) {
            return;
        }
        KTextEditor::Document *document = documentForPath(path);
        if (!document) {
            // Nothing to attach a banner to: the file is not open.
            return;
        }
        if (m_bannerPaths.contains(path)) {
            // One banner per file and review round, however many edits the
            // turn makes to it. The pending-edits-cleared signal resets the
            // set for the next round.
            return;
        }
        m_bannerPaths.insert(path);

        auto *message = new KTextEditor::Message(
            i18n("Kate AI changed this file. The change is queued for review in the Kate AI panel."),
            KTextEditor::Message::Information);
        message->setWordWrap(true);
        // The banner outlives the panel when its document is shared with
        // another window, so the actions go through a pointer that nulls out
        // with the view instead of capturing this directly.
        QPointer<KateAiView> self = this;
        auto *review = new QAction(i18n("Review"), message);
        connect(review, &QAction::triggered, this, [self]() {
            if (self) {
                self->showPanel();
            }
        });
        auto *reject = new QAction(i18n("Reject Changes"), message);
        connect(reject, &QAction::triggered, this, [self, path]() {
            if (self && self->m_chat) {
                self->m_chat->rejectPendingEdits(path);
            }
        });
        message->addAction(review);
        message->addAction(reject);
        document->postMessage(message);
    }


    void KateAiView::setupStatusBarPill()
    {
        if (!m_mainWindow) {
            return;
        }
        auto *mw = qobject_cast<QMainWindow *>(m_mainWindow->window());
        if (!mw || !mw->statusBar()) {
            return;
        }
        auto *btn = new QToolButton(mw->statusBar());
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setToolButtonStyle(Qt::ToolButtonTextOnly);
        btn->setToolTip(i18n("Click to open the Kate AI panel"));
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { padding: 1px 7px; border-radius: 3px; font-size: 11px; }"
            "QToolButton:hover { background: rgba(128,128,128,0.18); }"));
        connect(btn, &QToolButton::clicked, this, &KateAiView::showPanel);
        mw->statusBar()->addPermanentWidget(btn);
        m_statusPill = btn;
        updateStatusBarPill();
    }

    void KateAiView::updateStatusBarPill()
    {
        if (!m_statusPill) {
            return;
        }
        QStringList parts;
        const QString mode = (m_plugin && !m_plugin->settings().agentMode.isEmpty())
            ? m_plugin->settings().agentMode
            : u"code"_s;
        const bool busy = m_chat && m_chat->agent() && m_chat->agent()->isBusy();
        parts.append(busy ? i18n("Kate AI: %1 (working…)", mode) : i18n("Kate AI: %1", mode));
        if (m_chat && m_chat->editTracker()) {
            const int pending = m_chat->editTracker()->pendingEditCount();
            if (pending > 0) {
                parts.append(i18np("%1 edit pending", "%1 edits pending", pending));
            }
        }
        if (m_debug && m_debug->isAvailable()) {
            const DebugSessionState st = m_debug->sessionState();
            if (st == DebugSessionState::Paused) {
                parts.append(i18n("DBG paused"));
            } else if (st == DebugSessionState::Running) {
                parts.append(i18n("DBG running"));
            }
        }
        m_statusPill->setText(parts.join(u" · "_s));
    }

    void KateAiView::postDebugStopBanner(const DebugStopInfo &info)
    {
        KTextEditor::Document *document = !info.file.isEmpty() ? documentForPath(info.file) : nullptr;
        if (!document && m_mainWindow && m_mainWindow->activeView()) {
            document = m_mainWindow->activeView()->document();
        }
        if (!document) {
            return;
        }
        const QString detail = info.description.isEmpty() ? info.reason : (info.reason + u": "_s + info.description);
        auto *message = new KTextEditor::Message(
            i18n("Debugger stopped on crash/exception (%1). Kate AI can inspect the stack and local variables.", detail),
            KTextEditor::Message::Warning);
        message->setWordWrap(true);
        QPointer<KateAiView> self = this;
        auto *diagnose = new QAction(i18n("Diagnose with Kate AI"), message);
        connect(diagnose, &QAction::triggered, this, [self]() {
            if (self) {
                self->diagnoseDebugStop();
            }
        });
        message->addAction(diagnose);
        document->postMessage(message);
    }

    void KateAiView::diagnoseDebugStop()
    {
        showPanel();
        if (!m_chat) {
            return;
        }
        m_chat->applyMode(u"debug"_s);
        m_chat->ask(i18n(
            "Inspect the current live debugger stop using debug_status, debug_stack, and debug_locals. "
            "Explain the root cause of the stop/crash and propose a focused fix."));
    }

    void KateAiView::explainDebugStack()
    {
        showPanel();
        if (!m_chat) {
            return;
        }
        m_chat->ask(i18n(
            "Read the current debugger call stack (debug_stack) and active frame locals (debug_locals), "
            "and explain how execution reached this point and what each frame is doing."));
    }

    void KateAiView::fixDiagnostics()
    {
        showPanel();
        if (!m_chat) {
            return;
        }
        QString file;
        if (m_mainWindow && m_mainWindow->activeView() && m_mainWindow->activeView()->document()) {
            file = m_mainWindow->activeView()->document()->url().toLocalFile();
        }
        if (file.isEmpty()) {
            m_chat->ask(i18n("Inspect workspace diagnostics with lsp_diagnostics, explain the errors, and apply focused fixes."));
        } else {
            m_chat->ask(i18n("Inspect diagnostics for `%1` with lsp_diagnostics, explain each error/warning, and apply focused fixes.", file));
        }
    }

    void KateAiView::fixBuildErrors()
    {
        showPanel();
        if (!m_chat) {
            return;
        }
        m_chat->ask(i18n(
            "Inspect build targets with build_targets and compiler diagnostics with lsp_diagnostics "
            "(or run the build if needed), diagnose the build failures, and apply focused fixes."));
    }

    void KateAiView::generateLaunchJson()
    {
        const QString workspace = m_workspace.isEmpty() ? detectWorkspace(m_mainWindow) : m_workspace;
        if (workspace.isEmpty()) {
            return;
        }
        QString summary;
        const QJsonDocument doc = DebugProvider::generateLaunchConfig(workspace, &summary);
        const QString vscodeDir = QDir(workspace).absoluteFilePath(u".vscode"_s);
        const QString launchPath = QDir(vscodeDir).absoluteFilePath(u"launch.json"_s);

        if (QFile::exists(launchPath)) {
            const auto reply = QMessageBox::question(
                m_mainWindow ? m_mainWindow->window() : nullptr,
                i18n("Overwrite launch.json?"),
                i18n("%1 already exists.\n\nOverwrite it with an auto-generated Kate DAP launch configuration?\n(%2)",
                     launchPath, summary),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No);
            if (reply != QMessageBox::Yes) {
                openFileAtLine(launchPath, 1);
                return;
            }
        }

        QDir().mkpath(vscodeDir);
        QFile f(launchPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (m_mainWindow) {
                QVariantMap msg;
                msg.insert(u"type"_s, u"Error"_s);
                msg.insert(u"category"_s, i18n("Kate AI"));
                msg.insert(u"text"_s, i18n("Could not write %1.", launchPath));
                m_mainWindow->showMessage(msg);
            }
            return;
        }
        f.write(doc.toJson(QJsonDocument::Indented));
        f.close();

        if (m_debug) {
            m_debug->reloadLaunchJsonTargets();
        }
        openFileAtLine(launchPath, 1);
        if (m_mainWindow) {
            QVariantMap msg;
            msg.insert(u"type"_s, u"Information"_s);
            msg.insert(u"category"_s, i18n("Kate AI"));
            msg.insert(u"text"_s, i18n("Generated %1 (%2).", launchPath, summary));
            m_mainWindow->showMessage(msg);
        }
    }

    void KateAiView::snapshotDebugState()
    {
        if (!m_debug || !m_debug->isAvailable()) {
            if (m_mainWindow) {
                QVariantMap msg;
                msg.insert(u"type"_s, u"Warning"_s);
                msg.insert(u"category"_s, i18n("Kate AI"));
                msg.insert(u"text"_s, i18n("Kate's debugger plugin (kategdbplugin) is not active."));
                m_mainWindow->showMessage(msg);
            }
            return;
        }
        const DebugSnapshot snap = m_debug->captureSnapshot();
        const QList<DebugSnapshot> all = m_debug->snapshots();
        if (all.size() >= 2 && m_ecosystem) {
            const QString diffText = DebugProvider::diffSnapshots(all.at(all.size() - 2), all.last());
            m_ecosystem->openNativeTab(
                i18n("Debug Diff: %1 -> %2", all.at(all.size() - 2).label, all.last().label),
                i18n("Snapshot comparison (%1 captured total)", all.size()),
                diffText,
                true);
        } else if (m_mainWindow) {
            QVariantMap msg;
            msg.insert(u"type"_s, u"Information"_s);
            msg.insert(u"category"_s, i18n("Kate AI"));
            msg.insert(u"text"_s, i18n("Captured debugger snapshot '%1' (%2 frames, %3 locals). Capture another snapshot to diff.",
                                       snap.label, snap.frames.size(), snap.locals.size()));
            m_mainWindow->showMessage(msg);
        }
    }

    void KateAiView::openReviewDiffTab()
    {
        if (!m_ecosystem || !m_chat || !m_chat->editTracker()) {
            return;
        }
        EditTracker *tracker = m_chat->editTracker();
        const QList<EditEntry> entries = tracker->pendingEdits();
        if (entries.isEmpty()) {
            if (m_mainWindow) {
                QVariantMap msg;
                msg.insert(u"type"_s, u"Information"_s);
                msg.insert(u"category"_s, i18n("Kate AI"));
                msg.insert(u"text"_s, i18n("There are no pending AI edits to review."));
                m_mainWindow->showMessage(msg);
            }
            return;
        }
        QStringList blocks;
        for (const EditEntry &e : entries) {
            const QString rel = m_workspace.isEmpty() ? e.path : QDir(m_workspace).relativeFilePath(e.path);
            blocks.append(u"=== %1 (%2) ===\n%3"_s.arg(rel, e.toolName, e.diff));
        }
        QPointer<EditTracker> trackerPtr = tracker;
        m_ecosystem->openNativeTab(
            i18n("Kate AI Review Diff (%1)", entries.size()),
            i18np("%1 pending file change", "%1 pending file changes", entries.size()),
            blocks.join(u"\n\n"_s),
            true,
            [trackerPtr]() {
                if (trackerPtr) {
                    trackerPtr->acceptAll();
                }
            },
            [trackerPtr]() {
                if (trackerPtr) {
                    trackerPtr->rejectAll();
                }
            });
    }

    void KateAiView::openProjectGraphTab()
    {
        if (!m_ecosystem || !m_chat || !m_chat->agent() || !m_chat->agent()->getProjectGraph()) {
            return;
        }
        ProjectGraph *graph = m_chat->agent()->getProjectGraph();
        const QString summary = graph->contextSummary(200, 300, 3);
        m_ecosystem->openNativeTab(
            i18n("Project Graph"),
            i18n("Workspace: %1 (%2 nodes, %3 edges)", m_workspace, graph->getNodeCount(), graph->getEdgeCount()),
            summary.isEmpty() ? i18n("(Project graph is empty.)") : summary,
            false);
    }

    void KateAiView::toggleAnnotationGutter()
    {
        if (!m_assist) {
            return;
        }
        const bool next = !m_assist->annotationGutterEnabled();
        m_assist->setAnnotationGutterEnabled(next);
        if (m_annotationAction) {
            m_annotationAction->setChecked(next);
        }
    }

    void KateAiView::generateCommitMessage()
    {
        showPanel();
        if (!m_chat) {
            return;
        }
        m_chat->ask(i18n(
            "Inspect `git status` and `git diff --staged` (or `git diff` if nothing is staged) via bash, "
            "and draft a clear, concise commit message summarizing the changes and why they were made."));
    }

    void KateAiView::explainGitBlame()
    {
        showPanel();
        if (!m_chat || !m_mainWindow || !m_mainWindow->activeView() || !m_mainWindow->activeView()->document()) {
            return;
        }
        KTextEditor::View *view = m_mainWindow->activeView();
        const QString path = view->document()->url().toLocalFile();
        if (path.isEmpty()) {
            return;
        }
        int startLine = view->cursorPosition().line() + 1;
        int endLine = startLine;
        if (view->selection()) {
            const KTextEditor::Range r = view->selectionRange();
            startLine = r.start().line() + 1;
            endLine = qMax(startLine, r.end().line() + 1);
        }
        m_chat->ask(i18n(
            "Run `git log -n 5 -L %1,%2:%3` (or `git blame -L %1,%2 %3`) via bash and explain the history and rationale behind these lines.",
            startLine, endLine, path));
    }

    void KateAiView::analyzeBacktrace()
    {
        showPanel();
        if (!m_chat) {
            return;
        }
        const QString bt = m_ecosystem ? m_ecosystem->activeBacktraceText().trimmed() : QString();
        if (!bt.isEmpty()) {
            m_chat->ask(i18n(
                "Analyze this crash backtrace from Kate's Backtrace Browser, locate the relevant workspace files, and identify the likely root cause and fix:\n\n```\n%1\n```",
                bt.left(8000)));
        } else {
            m_chat->ask(i18n(
                "No backtrace is loaded in Kate's Backtrace Browser. Check debug_stack if a debug session is paused, or ask me for the stack trace to analyze."));
        }
    }

    void KateAiView::explainTerminalSelection()
    {
        showPanel();
        if (!m_chat) {
            return;
        }
        const QString sel = m_ecosystem ? m_ecosystem->terminalContext().trimmed() : QString();
        if (!sel.isEmpty()) {
            m_chat->ask(i18n(
                "Explain this output from Kate's Konsole terminal and suggest fixes for any errors:\n\n```\n%1\n```",
                sel.left(6000)));
        } else {
            m_chat->ask(i18n(
                "Nothing is currently selected in Kate's Konsole terminal. Select some terminal output and run this action again, or paste the error here."));
        }
    }

    bool KateAiView::setBreakpointAtCursor(const QString &condition, QString *message)
    {
        if (!m_debug || !m_debug->isAvailable()) {
            if (message) {
                *message = i18n("Kate's debugger plugin (kategdbplugin) is not available.");
            }
            return false;
        }
        if (!m_mainWindow || !m_mainWindow->activeView() || !m_mainWindow->activeView()->document()) {
            if (message) {
                *message = i18n("No active editor document.");
            }
            return false;
        }
        KTextEditor::View *view = m_mainWindow->activeView();
        const QString path = view->document()->url().toLocalFile();
        const int line = view->cursorPosition().line() + 1;
        if (path.isEmpty()) {
            if (message) {
                *message = i18n("Save the document before setting a breakpoint.");
            }
            return false;
        }
        const DebugActionResult res = m_debug->setBreakpoint(path, line, condition);
        if (message) {
            *message = res.output;
        }
        return res.ok;
    }

} // namespace KateAi
