/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.windu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "suggestionwidget.h"

#include "agenttext.h"
#include "chattheme.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

SuggestionWidget::SuggestionWidget(const QString &suggestionId, QWidget *parent)
    : QWidget(parent)
    , m_suggestionId(suggestionId)
{
    setObjectName(u"suggestionWidget"_s);
    // A plain QWidget subclass silently ignores a stylesheet background-color
    // without this, which would leave the card transparent.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setSpacing(6);

    // A fixed lightbulb badge: the card stays recognisable after the state
    // glyph changes.
    auto *badge = new QLabel(u"\u{1F4A1}"_s, this);
    badge->setToolTip(i18n("Suggestion from the agent"));
    badge->setStyleSheet(QStringLiteral("font-size: 13px; color: %1;").arg(ChatTheme::warning()));
    header->addWidget(badge);

    m_icon = new QLabel(u"\u{2713}"_s, this);
    m_icon->setStyleSheet(u"font-size: 12px;"_s);
    header->addWidget(m_icon);

    m_title = new QLabel(i18n("Suggestion"), this);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setStyleSheet(QStringLiteral("QLabel { color: %1; font-size: 11px; font-weight: 600; }")
                               .arg(ChatTheme::textPrimary()));
    header->addWidget(m_title);

    // Where the suggestion points, so the user can tell at a glance whether it
    // is about the file they are looking at.
    m_anchor = new QLabel(this);
    m_anchor->setTextFormat(Qt::PlainText);
    m_anchor->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(ChatTheme::textMuted()));
    m_anchor->setVisible(false);
    header->addWidget(m_anchor);

    m_status = new QLabel(this);
    m_status->setTextFormat(Qt::PlainText);
    m_status->setStyleSheet(QStringLiteral("color: %1; font-size: 10px;").arg(ChatTheme::textMuted()));
    header->addWidget(m_status);
    header->addStretch();

    m_dismiss = new QPushButton(i18n("Dismiss"), this);
    m_dismiss->setCursor(Qt::PointingHandCursor);
    m_dismiss->setFixedHeight(20);
    m_dismiss->setStyleSheet(
        QStringLiteral(
            "QPushButton { background: transparent; color: %1; border: 1px solid %2;"
            " border-radius: 5px; padding: 0 8px; font-size: 10px; }"
            "QPushButton:hover { background-color: %3; }")
        .arg(ChatTheme::textMuted(), ChatTheme::border(), ChatTheme::hoverBg()));
    connect(m_dismiss, &QPushButton::clicked, this, [this] {
        Q_EMIT dismissed(m_suggestionId);
    });
    header->addWidget(m_dismiss);

    m_accept = new QPushButton(i18n("Accept"), this);
    m_accept->setCursor(Qt::PointingHandCursor);
    m_accept->setFixedHeight(20);
    m_accept->setStyleSheet(
        QStringLiteral(
            "QPushButton { background: %1; color: #ffffff; border: none;"
            " border-radius: 5px; padding: 0 10px; font-size: 10px; font-weight: 600; }"
            "QPushButton:hover { background: %2; }")
        .arg(ChatTheme::accent(), ChatTheme::accentHover()));
    connect(m_accept, &QPushButton::clicked, this, [this] {
        m_decided = true;
        Q_EMIT accepted(m_suggestionId, m_path, m_line);
    });
    header->addWidget(m_accept);

    root->addLayout(header);

    // The suggestion itself, rendered as markdown so a snippet inside it gets
    // the same highlighting it would have in the editor.
    m_body = new QTextBrowser(this);
    m_body->setOpenExternalLinks(true);
    m_body->setFrameShape(QFrame::NoFrame);
    m_body->setStyleSheet(
        QStringLiteral("QTextBrowser { background: transparent; border: none; color: %1; font-size: 11px; padding: 0; }")
            .arg(ChatTheme::textPrimary()));
    m_body->setMinimumHeight(0);
    m_body->setVisible(false);
    root->addWidget(m_body);

    applyStyle();
    updateHeader();
}

SuggestionWidget::~SuggestionWidget() = default;

void SuggestionWidget::setSuggestion(const QString &text, const QString &path, int line)
{
    m_path = path;
    m_line = line;
    m_failed = false;
    m_running = false;
    m_body->setDocument(makeMarkdownDocument(closeMarkdown(text)));
    m_body->setVisible(true);
    applyStyle();
    updateHeader();
    reflowNow();
}

void SuggestionWidget::setRunning()
{
    if (m_failed || m_decided) {
        return;
    }
    m_running = true;
    applyStyle();
    updateHeader();
}

void SuggestionWidget::setError(const QString &message)
{
    m_failed = true;
    m_running = false;
    m_body->setDocument(makeMarkdownDocument(closeMarkdown(message)));
    m_body->setVisible(true);
    // A suggestion that never arrived cannot be accepted or dismissed.
    m_accept->setVisible(false);
    m_dismiss->setVisible(false);
    applyStyle();
    updateHeader();
    reflowNow();
}

void SuggestionWidget::markDecided()
{
    m_decided = true;
    m_accept->setVisible(false);
    m_dismiss->setVisible(false);
    m_status->setText(i18n("from an earlier turn"));
    applyStyle();
}

void SuggestionWidget::updateHeader()
{
    if (m_failed) {
        m_status->setText(i18n("failed"));
        return;
    }
    if (m_decided) {
        m_status->setText(i18n("accepted"));
        return;
    }
    if (m_running) {
        m_status->setText(i18n("posting…"));
        return;
    }
    if (!m_path.isEmpty()) {
        m_anchor->setText(m_line > 0 ? u"%1:%2"_s.arg(m_path).arg(m_line) : m_path);
        m_anchor->setVisible(true);
    }
    m_status->setText(i18n("posted"));
}

void SuggestionWidget::applyStyle()
{
    QString border = ChatTheme::border();
    QString glyph = u"\u{2713}"_s;
    if (m_failed) {
        border = ChatTheme::danger();
        glyph = u"\u{2717}"_s;
    } else if (m_decided) {
        border = ChatTheme::success();
    } else if (m_running) {
        // Live rather than settled: the same accent the running sub-agent card
        // uses, so a card still waiting on its tool reads the same way.
        border = ChatTheme::accent();
        glyph = u"\u{25D0}"_s;
    } else {
        border = ChatTheme::warning();
    }

    m_icon->setText(glyph);
    m_icon->setStyleSheet(QStringLiteral("font-size: 12px; color:%1;").arg(border));

    QString sheet = ChatTheme::agentTaskCard();
    sheet += QStringLiteral("QWidget#suggestionWidget { border-color: %1; }"
                            "QWidget#suggestionWidget QLabel { background: transparent; }")
                 .arg(border);
    setStyleSheet(sheet);
}

void SuggestionWidget::reflowNow()
{
    if (m_body->isVisible()) {
        m_body->setMinimumHeight(0);
        m_body->document()->setTextWidth(m_body->viewport()->width());
        m_body->setMinimumHeight(m_body->document()->size().height());
    }
    updateGeometry();
}

} // namespace KateAi
