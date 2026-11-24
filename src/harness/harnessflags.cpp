/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "harnessflags.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QProcessEnvironment>

using namespace Qt::Literals::StringLiterals;

namespace KateAi
{

namespace
{

struct FlagRef {
    const char *name;
    bool HarnessFlags::*member;
};

const FlagRef kFlags[] = {
    {"stablePromptPrefix", &HarnessFlags::stablePromptPrefix},
    {"resultBudget", &HarnessFlags::resultBudget},
    {"contextReport", &HarnessFlags::contextReport},
    {"eventLog", &HarnessFlags::eventLog},
    {"middleware", &HarnessFlags::middleware},
    {"verificationGate", &HarnessFlags::verificationGate},
    {"policyV2", &HarnessFlags::policyV2},
    {"hooks", &HarnessFlags::hooks},
    {"promptV2", &HarnessFlags::promptV2},
    {"contextV2", &HarnessFlags::contextV2},
    {"memoryV2", &HarnessFlags::memoryV2},
    {"toolSearch", &HarnessFlags::toolSearch},
    {"skills", &HarnessFlags::skills},
    {"agentsV2", &HarnessFlags::agentsV2},
    {"inspector", &HarnessFlags::inspector},
    {"orchestration", &HarnessFlags::orchestration},
    {"telemetry", &HarnessFlags::telemetry},
};

} // namespace

void HarnessFlags::applyOverrides(const QString &overrides)
{
    const QStringList items = overrides.split(u',', Qt::SkipEmptyParts);
    for (const QString &raw : items) {
        QString item = raw.trimmed();
        bool value = true;
        if (item.startsWith(u'+')) {
            item = item.mid(1);
        } else if (item.startsWith(u'-')) {
            value = false;
            item = item.mid(1);
        }
        for (const FlagRef &flag : kFlags) {
            if (item == QLatin1String(flag.name)) {
                this->*flag.member = value;
                break;
            }
        }
    }
}

HarnessFlags HarnessFlags::load()
{
    HarnessFlags flags;

    const KConfigGroup group(KSharedConfig::openConfig(), u"Harness"_s);
    for (const FlagRef &flag : kFlags) {
        const QString key = QString::fromLatin1(flag.name);
        flags.*flag.member = group.readEntry(key, flags.*flag.member);
    }

    // The environment form is intentionally last: a developer can always
    // override a persisted setting for one run.
    flags.applyOverrides(QProcessEnvironment::systemEnvironment().value(u"KATEAI_HARNESS_FLAGS"_s));
    return flags;
}

bool HarnessFlags::enabled(const QString &name) const
{
    for (const FlagRef &flag : kFlags) {
        if (name == QLatin1String(flag.name)) {
            return this->*flag.member;
        }
    }
    return false;
}

QStringList HarnessFlags::enabledNames() const
{
    QStringList names;
    for (const FlagRef &flag : kFlags) {
        if (this->*flag.member) {
            names.append(QString::fromLatin1(flag.name));
        }
    }
    return names;
}

} // namespace KateAi
