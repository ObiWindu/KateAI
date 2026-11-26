/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "transcriptvirtualizer.h"

#include <QLabel>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestTranscriptVirtualizer : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void longTranscriptsMountOnlyTheVisibleWindow()
    {
        QWidget root;
        root.resize(700, 500);
        auto *rootLayout = new QVBoxLayout(&root);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        auto *scroll = new QScrollArea(&root);
        scroll->setWidgetResizable(true);
        auto *container = new QWidget;
        auto *layout = new QVBoxLayout(container);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);
        layout->addStretch();
        scroll->setWidget(container);
        rootLayout->addWidget(scroll);
        root.show();
        QTest::qWait(20);

        TranscriptVirtualizer virtualizer(scroll, layout);
        QSignalSpy evicted(&virtualizer, &TranscriptVirtualizer::entryWillEvict);
        QList<TranscriptVirtualizer::Entry> entries;
        int created = 0;
        for (int i = 0; i < 1000; ++i) {
            TranscriptVirtualizer::Entry entry;
            entry.key = u"message:%1"_s.arg(i);
            entry.toolKeys = {u"call-%1"_s.arg(i)};
            entry.estimatedHeight = 90;
            entry.pinned = i == 900;
            entry.materialize = [&virtualizer, &created, i, container] {
                ++created;
                auto *label = new QLabel(u"entry %1"_s.arg(i), container);
                label->setObjectName(u"heavyCard"_s);
                label->setFixedHeight(70);
                virtualizer.appendToCurrent(label);
            };
            entries.append(entry);
        }
        virtualizer.setEntries(entries);
        QTest::qWait(30);
        QVERIFY(virtualizer.isActive());
        QCOMPARE(virtualizer.entryCount(), 1000);
        QVERIFY(virtualizer.materializedEntryCount() > 0);
        QVERIFY(virtualizer.materializedEntryCount() < 80);
        QCOMPARE(container->findChildren<QLabel *>(u"heavyCard"_s).size(),
                 virtualizer.materializedEntryCount());
        const QStringList aliases = virtualizer.toolAliases();
        QCOMPARE(aliases.size(), 1000);
        QCOMPARE(virtualizer.toolWidgetKey(aliases.at(500)), u"call-500"_s);
        QWidget *target = virtualizer.ensureAliasVisible(aliases.at(500));
        QVERIFY(target);
        QCOMPARE(qobject_cast<QLabel *>(target)->text(), u"entry 500"_s);
        QTest::qWait(20);
        QVERIFY(virtualizer.materializedEntryCount() < 80);

        auto *scrollBar = scroll->verticalScrollBar();
        scrollBar->setValue(0);
        QTest::qWait(30);
        QWidget *first = virtualizer.ensureAliasVisible(u"message:0"_s);
        QVERIFY(first);
        QCOMPARE(qobject_cast<QLabel *>(first)->text(), u"entry 0"_s);
        QVERIFY(virtualizer.materializedEntryCount() < 80);
        QVERIFY(evicted.count() > 0);

        auto *live = new QLabel(u"live tail"_s, container);
        layout->insertWidget(virtualizer.appendIndex(), live);
        scrollBar->setValue(scrollBar->maximum());
        QTest::qWait(30);
        QVERIFY(live->isVisible());

        QVERIFY(virtualizer.ensureAliasVisible(u"message:0"_s));
        virtualizer.clear();
        QVERIFY(!virtualizer.isActive());
        QCOMPARE(virtualizer.entryCount(), 0);
        QCOMPARE(virtualizer.materializedEntryCount(), 0);
        QVERIFY(container->findChildren<QLabel *>(u"heavyCard"_s).isEmpty());
        QVERIFY(live->isVisible());
    }

    void shortTranscriptsKeepTheExistingEagerPath()
    {
        QWidget root;
        auto *scroll = new QScrollArea(&root);
        auto *container = new QWidget;
        auto *layout = new QVBoxLayout(container);
        layout->addStretch();
        scroll->setWidget(container);
        TranscriptVirtualizer virtualizer(scroll, layout, 256);
        QList<TranscriptVirtualizer::Entry> entries;
        for (int i = 0; i < 20; ++i) {
            TranscriptVirtualizer::Entry entry;
            entry.key = u"short:%1"_s.arg(i);
            entry.materialize = [&virtualizer, i, container] {
                auto *label = new QLabel(u"short %1"_s.arg(i), container);
                virtualizer.appendToCurrent(label);
            };
            entries.append(entry);
        }
        virtualizer.setEntries(entries);
        QVERIFY(!virtualizer.isActive());
        QCOMPARE(virtualizer.entryCount(), 0);
        QVERIFY(container->findChildren<QLabel *>().isEmpty());
    }
};

QTEST_MAIN(TestTranscriptVirtualizer)
#include "test_transcriptvirtualizer.moc"
