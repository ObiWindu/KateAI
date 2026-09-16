#pragma once

#include "types.h"

#include <KTextEditor/ConfigPage>

class QComboBox;
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;
class QDoubleSpinBox;

namespace KateAi
{

class KateAiPlugin;

class KateAiConfigPage : public KTextEditor::ConfigPage
{
    Q_OBJECT

public:
    KateAiConfigPage(QWidget *parent, KateAiPlugin *plugin);

    ~KateAiConfigPage() override;

    QString name() const override;
    QString fullName() const override;
    QIcon icon() const override;

    void apply() override;
    void reset() override;
    void defaults() override;

private:
    KateAiPlugin *m_plugin = nullptr;

    // Providers
    QComboBox *m_provider = nullptr;
    QLineEdit *m_grokKey = nullptr;
    QLineEdit *m_openaiKey = nullptr;
    QLineEdit *m_openrouterKey = nullptr;
    QLineEdit *m_openaiCompatibleKey = nullptr;
    QLineEdit *m_claudeCompatibleKey = nullptr;
    QLineEdit *m_grokModel = nullptr;
    QLineEdit *m_openaiModel = nullptr;
    QLineEdit *m_openrouterModel = nullptr;
    QLineEdit *m_openaiCompatibleModel = nullptr;
    QLineEdit *m_claudeCompatibleModel = nullptr;
    QLineEdit *m_openaiCompatibleUrl = nullptr;
    QLineEdit *m_claudeCompatibleUrl = nullptr;

    // Security
    QComboBox *m_permission = nullptr;
    QComboBox *m_sandbox = nullptr;
    QSpinBox *m_timeout = nullptr;
    QPlainTextEdit *m_deny = nullptr;

    // Agent
    QSpinBox *m_maxIter = nullptr;
    QSpinBox *m_maxModelRequests = nullptr;
    QSpinBox *m_requestsPerMinute = nullptr;
    QCheckBox *m_planMode = nullptr;
    QCheckBox *m_projectInstructions = nullptr;
    QPlainTextEdit *m_system = nullptr;
    QComboBox *m_speed = nullptr;
    QCheckBox *m_thinkingMode = nullptr;

    // Compression
    QSpinBox *m_compressionLevel = nullptr;
    QSpinBox *m_maxGraphNodes = nullptr;
    QSpinBox *m_maxGraphEdges = nullptr;
    QCheckBox *m_compressGraph = nullptr;
    QCheckBox *m_includeFileContents = nullptr;
    QSpinBox *m_maxFileContentLength = nullptr;
    QCheckBox *m_compressEditorContext = nullptr;
    QSpinBox *m_maxEditorContextLength = nullptr;
    QCheckBox *m_compressProjectInstructions = nullptr;
    QSpinBox *m_maxProjectInstructionsLength = nullptr;
    QCheckBox *m_compressSystemPrompt = nullptr;
    QSpinBox *m_maxSystemPromptLength = nullptr;

    // Optimal Intelligence Parameters
    QDoubleSpinBox *m_temperature = nullptr;
    QDoubleSpinBox *m_topP = nullptr;
    QSpinBox *m_maxTokens = nullptr;
    QComboBox *m_reasoningEffort = nullptr;
    QCheckBox *m_selfCritique = nullptr;
    QCheckBox *m_parallelToolCalls = nullptr;
    QComboBox *m_verbosity = nullptr;
};

} // namespace KateAi
