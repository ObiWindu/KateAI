#include "permissionbar.h"

#include <KLocalizedString>

#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

PermissionBar::PermissionBar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);

    m_title = new QLabel(this);
    m_title->setWordWrap(true);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    layout->addWidget(m_title);

    m_details = new QPlainTextEdit(this);
    m_details->setReadOnly(true);
    m_details->setMaximumHeight(140);
    m_details->setPlaceholderText(i18n("Details"));
    layout->addWidget(m_details);

    auto *buttons = new QHBoxLayout;
    auto *allow = new QPushButton(i18n("Allow"), this);
    auto *session = new QPushButton(i18n("Allow for session"), this);
    auto *deny = new QPushButton(i18n("Deny"), this);
    deny->setDefault(false);
    allow->setDefault(true);
    buttons->addWidget(allow);
    buttons->addWidget(session);
    buttons->addWidget(deny);
    buttons->addStretch();
    layout->addLayout(buttons);

    connect(allow, &QPushButton::clicked, this, [this]() {
        hideBar();
        Q_EMIT decided(PermissionDecision::AllowOnce);
    });
    connect(session, &QPushButton::clicked, this, [this]() {
        hideBar();
        Q_EMIT decided(PermissionDecision::AllowSession);
    });
    connect(deny, &QPushButton::clicked, this, [this]() {
        hideBar();
        Q_EMIT decided(PermissionDecision::Deny);
    });

    hide();
}

void PermissionBar::showRequest(const PermissionRequest &request)
{
    m_title->setText(i18n("Permission needed: %1", request.summary));
    m_details->setPlainText(request.details);
    show();
}

void PermissionBar::hideBar()
{
    hide();
    m_details->clear();
}

} // namespace KateAi
