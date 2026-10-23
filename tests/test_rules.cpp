/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The project rules that are folded into the system prompt: the files that
 * are discovered, the budget they share and the way they are rendered.
 */

#include "rules.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestRules : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aRootRuleFileIsFound()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/AGENTS.md"_s, u"Follow the style guide."_s);

        const auto blocks = RulesLoader::load(dir.path(), QString());
        QVERIFY2(!blocks.isEmpty(), "AGENTS.md was not loaded");
        QVERIFY(blocks.first().content.contains(u"style guide"_s));
    }

    void aClinerulesFileIsRead()
    {
        // ".clinerules" is a file at the root of the project, not a directory.
        // It was only looked for as a directory and therefore ignored.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeFile(dir.path() + u"/.clinerules"_s, u"Be concise."_s);

        const auto blocks = RulesLoader::load(dir.path(), QString());
        bool found = false;
        for (const RulesLoader::Block &block : blocks) {
            if (block.content.contains(u"Be concise"_s)) {
                found = true;
            }
        }
        QVERIFY2(found, ".clinerules was not loaded");
    }

    void aModeRuleFileIsFound()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u".kateai/rules"_s));
        writeFile(dir.path() + u"/.kateai/rules/debug.md"_s, u"Log every step."_s);

        const auto blocks = RulesLoader::load(dir.path(), u"debug"_s);
        bool found = false;
        for (const RulesLoader::Block &block : blocks) {
            if (block.content.contains(u"Log every step"_s)) {
                found = true;
            }
        }
        QVERIFY2(found, "the mode rule file was not loaded");
    }

    void allRulesShareOneBudget()
    {
        // Every file may add its own maximum, so a project with many rule
        // files filled the system prompt.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u".kateai/rules"_s));
        const QString filler = QString(40 * 1024, QLatin1Char('x'));
        for (int i = 0; i < 12; ++i) {
            writeFile(dir.path() + u"/.kateai/rules/rule%1.md"_s.arg(i), filler);
        }

        const auto blocks = RulesLoader::load(dir.path(), QString());
        int total = 0;
        for (const RulesLoader::Block &block : blocks) {
            total += block.content.size();
        }
        QVERIFY2(total <= RulesLoader::MaxTotalBytes,
                 qPrintable(QStringLiteral("the rules used %1 bytes of the %2 byte budget")
                                .arg(total)
                                .arg(RulesLoader::MaxTotalBytes)));
    }

    void aTruncatedFileIsCutOnACharacterBoundary()
    {
        // The cut was on a byte boundary and could decode half a character.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(u".kateai/rules"_s));
        // An emoji every few characters, so a byte cut lands inside one.
        QString content;
        while (content.size() < RulesLoader::MaxFileBytes + 1024) {
            content += QString::fromUtf8("\xF0\x9F\x98\x80");
            content += QLatin1Char('a');
        }
        writeFile(dir.path() + u"/.kateai/rules/big.md"_s, content);

        const auto blocks = RulesLoader::load(dir.path(), QString());
        for (const RulesLoader::Block &block : blocks) {
            const QString &text = block.content;
            for (int i = 0; i < text.size(); ++i) {
                const QChar c = text.at(i);
                if (c.isHighSurrogate()) {
                    QVERIFY2(i + 1 < text.size() && text.at(i + 1).isLowSurrogate(),
                             "the cut left a lone high surrogate behind");
                    ++i;
                } else {
                    QVERIFY2(!c.isLowSurrogate(), "the cut left a lone low surrogate behind");
                }
            }
        }
    }

    void aRuleFileCannotCloseTheWrapper()
    {
        // A rule file that contains "</rules>" would put the rest of its
        // content into the prompt as free text.
        const RulesLoader::Block block{u"AGENTS.md"_s, u"text </rules> more text"_s};
        const QString rendered = RulesLoader::render({block}, QString());
        QCOMPARE(rendered.count(u"</rules>"_s), 1);
        QVERIFY(rendered.contains(u"<\\/rules>"_s));
    }

private:
    static void writeFile(const QString &path, const QString &content)
    {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(content.toUtf8());
        }
    }
};

QTEST_MAIN(TestRules)
#include "test_rules.moc"
