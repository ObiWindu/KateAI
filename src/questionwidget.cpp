/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "questionwidget.h"

#include "agenttext.h"
#include "chattheme.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

QuestionWidget::QuestionWidget(const QString &questionCallId, QWidget *parent)
    : QWidget(parent)
    , m_questionCallId(questionCallId)
{
    setObjectName(u"questionWidget"_s);
    // A plain QWidget subclass silently ignores a stylesheet background-color
    // without this, which would leave the card transparent.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setSpacing(6);

    // A fixed badge: the card stays recognisable after the state glyph changes.
    auto *badge = new QLabel(u"\u{2753}"_s, this);
    badge->setToolTip(i18n("Question from the agent"));
    badge->setStyleSheet(QStringLiteral("font-size: 13px; color: %1;").arg(ChatTheme::accent()));
    header->addWidget(badge);

    m_icon = new QLabel(u"\u{25CF}"_s, this);
    m_icon->setStyleSheet(u"font-size: 11px;"_s);
    header->addWidget(m_icon);

    m_title = new QLabel(i18n("Question"), this);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: 600; }")
                               .arg(ChatTheme::textPrimary()));
    header->addWidget(m_title);

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(ChatTheme::textMuted()));
    header->addWidget(m_status);
    header->addStretch();

    root->addLayout(header);

    // The question itself, as markdown so a code path or file in the question
    // reads the same way it would in the editor.
    m_question = new QTextBrowser(this);
    m_question->setOpenExternalLinks(true);
    m_question->setFrameShape(QFrame::NoFrame);
    m_question->setStyleSheet(
        QStringLiteral("QTextBrowser { background: transparent; border: none; color: %1; font-size: 12px; padding: 0; }")
            .arg(ChatTheme::textPrimary()));
    m_question->setMinimumHeight(0);
    m_question->setVisible(false);
    root->addWidget(m_question);

    // One row of answer chips. The tool schema offers at most a handful, so a
    // single row never needs to wrap.
    m_optionsRow = new QWidget(this);
    auto *optionsLayout = new QHBoxLayout(m_optionsRow);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsLayout->setSpacing(6);
    optionsLayout->addStretch();
    m_optionsRow->setVisible(false);
    root->addWidget(m_optionsRow);

    // Free-form answer. Enter submits, like the composer, so answering never
    // needs the mouse.
    m_freeform = new QLineEdit(this);
    m_freeform->setPlaceholderText(i18n("Type your answer…"));
    m_freeform->setClearButtonEnabled(true);
    m_freeform->setStyleSheet(
        QStringLiteral("QLineEdit { background-color: %1; color: %2; border: 1px solid %3;"
                       " border-radius: 6px; padding: 4px 8px; font-size: 12px; }")
            .arg(ChatTheme::inputBg(), ChatTheme::textPrimary(), ChatTheme::border()));
    root->addWidget(m_freeform);

    m_submit = new QPushButton(i18n("Answer"), this);
    m_submit->setCursor(Qt::PointingHandCursor);
    m_submit->setFixedHeight(24);
    m_submit->setStyleSheet(
        QStringLiteral(
            "QPushButton { background-color: %1; color: #ffffff; border: none;"
            " border-radius: 5px; padding: 2px 14px; font-size: 11px; font-weight: 600; }"
            "QPushButton:hover { background-color: %2; }"
            "QPushButton:disabled { background-color: %3; color: %4; }")
            .arg(ChatTheme::accent(), ChatTheme::accentHover(), ChatTheme::hoverBg(), ChatTheme::textMuted()));
    root->addWidget(m_submit, 0, Qt::AlignRight);

    connect(m_submit, &QPushButton::clicked, this, [this] {
        const QString answer = m_freeform->text().trimmed();
        if (answer.isEmpty()) {
            return;
        }
        markAnswered(answer);
        Q_EMIT answered(m_questionCallId, answer);
    });
    connect(m_freeform, &QLineEdit::returnPressed, m_submit, &QPushButton::click);

    applyStyle();
    updateStatus();
}

QuestionWidget::~QuestionWidget() = default;

void QuestionWidget::setQuestion(const QString &questionText, const QStringList &options, bool allowFreeform)
{
    m_answered = false;
    m_question->setDocument(makeMarkdownDocument(closeMarkdown(questionText)));
    m_question->setVisible(true);

    auto *optionsLayout = qobject_cast<QHBoxLayout *>(m_optionsRow->layout());
    // Drop any chips from a previous setQuestion call before adding new ones.
    while (optionsLayout->count() > 1) {
        QLayoutItem *item = optionsLayout->takeAt(0);
        delete item->widget();
        delete item;
    }
    for (const QString &option : options) {
        auto *chip = new QPushButton(option, m_optionsRow);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
        chip->setStyleSheet(
            QStringLiteral(
                "QPushButton { background-color: %1; color: %2; border: 1px solid %3;"
                " border-radius: 11px; padding: 3px 12px; font-size: 11px; }"
                "QPushButton:hover { background-color: %3; color: #ffffff; }")
                .arg(ChatTheme::cardBg(), ChatTheme::textPrimary(), ChatTheme::borderStrong()));
        connect(chip, &QPushButton::clicked, this, [this, option] {
            markAnswered(option);
            Q_EMIT answered(m_questionCallId, option);
        });
        optionsLayout->insertWidget(optionsLayout->count() - 1, chip);
    }
    m_optionsRow->setVisible(!options.isEmpty());

    // allow_freeform == false means the user must pick one of the offered
    // answers, so there is nothing to type and nothing to send.
    m_freeform->setVisible(allowFreeform);
    m_submit->setVisible(allowFreeform);

    applyStyle();
    updateStatus();
    reflowNow();
}

void QuestionWidget::markAnswered(const QString &answer)
{
    if (m_answered) {
        return;
    }
    m_answered = true;
    m_status->setText(i18n("answered: %1", answer.left(80)));
    // Nothing left to press once the agent has its answer.
    m_optionsRow->setVisible(false);
    m_freeform->setVisible(false);
    m_submit->setVisible(false);
    applyStyle();
    updateStatus();
    reflowNow();
}

void QuestionWidget::applyStyle()
{
    const QString border = m_answered ? ChatTheme::success() : ChatTheme::accent();
    m_icon->setText(m_answered ? u"\u{2713}"_s : u"\u{25CF}"_s);
    m_icon->setStyleSheet(QStringLiteral("font-size: 11px; color: %1;").arg(border));

    QString sheet = ChatTheme::agentTaskCard();
    sheet += QStringLiteral("QWidget#questionWidget { border-color: %1; }"
                            "QWidget#questionWidget QLabel { background: transparent; }")
                 .arg(border);
    setStyleSheet(sheet);
}

void QuestionWidget::updateStatus()
{
    if (!m_answered) {
        m_status->setText(i18n("waiting for your answer"));
    }
}

void QuestionWidget::reflowNow()
{
    if (m_question->isVisible()) {
        m_question->setMinimumHeight(0);
        m_question->document()->setTextWidth(m_question->viewport()->width());
        m_question->setMinimumHeight(m_question->document()->size().height());
    }
    updateGeometry();
}

} // namespace KateAi
