#pragma once

#include "documentbridge.h"

#include <KTextEditor/MainWindow>
#include <KXMLGUIClient>

#include <QObject>
#include <QPointer>

class QDialog;

namespace KateAi
{
class ChatWidget;
class KateAiPlugin;

class KateAiView : public QObject, public KXMLGUIClient
{
    Q_OBJECT

public:
    KateAiView(KateAiPlugin *plugin, KTextEditor::MainWindow *mainWindow);
    ~KateAiView() override;

    void showPanel();
    void newChat();
    void askSelection();
    void showConfiguration();

private:
    void addEditorContextActions(KTextEditor::View *view, const QList<QAction *> &actions);
    void askSelectionWithInstruction(const QString &instruction);
    QString editorContext() const;
    void refreshWorkspace();

    KateAiPlugin *m_plugin = nullptr;
    KTextEditor::MainWindow *m_mainWindow = nullptr;
    QPointer<QWidget> m_toolView;
    QPointer<QDialog> m_configDialog;
    ChatWidget *m_chat = nullptr;
    DiskDocumentBridge m_bridge;
};

} // namespace KateAi
