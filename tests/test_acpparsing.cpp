/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "acpclient.h"
#include "types.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace Qt::Literals::StringLiterals;
using namespace KateAi;

class TestAcpParsing : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void rawInputObject()
    {
        AcpClient client;
        const QJsonObject call{
            {u"toolCallId"_s, u"t1"_s},
            {u"kind"_s, u"edit"_s},
            {u"rawInput"_s, QJsonObject{{u"path"_s, u"/a.cpp"_s}, {u"old_string"_s, u"old"_s}, {u"new_string"_s, u"new"_s}}},
        };
        const PermissionRequest r = client.permissionFromToolCall(call);
        QCOMPARE(r.path, u"/a.cpp"_s);
        QVERIFY(r.describeDiff.contains(u"+new"_s));
    }

    void rawInputString()
    {
        AcpClient client;
        const QJsonObject call{
            {u"toolCallId"_s, u"t2"_s},
            {u"kind"_s, u"edit"_s},
            {u"rawInput"_s, u"{\"path\":\"/b.cpp\",\"oldText\":\"x\",\"newText\":\"y\"}"_s},
        };
        const PermissionRequest r = client.permissionFromToolCall(call);
        QCOMPARE(r.path, u"/b.cpp"_s);
        QVERIFY(r.describeDiff.contains(u"+y"_s));
    }

    void rawInputArray()
    {
        AcpClient client;
        const QJsonObject call{
            {u"toolCallId"_s, u"t3"_s},
            {u"kind"_s, u"edit"_s},
            {u"rawInput"_s, QJsonArray{QJsonObject{{u"path"_s, u"/c.cpp"_s}}, QJsonObject{{u"old_string"_s, u"a"_s}, {u"new_string"_s, u"b"_s}}}},
        };
        const PermissionRequest r = client.permissionFromToolCall(call);
        QCOMPARE(r.path, u"/c.cpp"_s);
        QVERIFY(r.describeDiff.contains(u"+b"_s));
    }

    void contentBlockDiff()
    {
        AcpClient client;
        const QJsonObject call{
            {u"toolCallId"_s, u"t4"_s},
            {u"kind"_s, u"edit"_s},
            {u"content"_s, QJsonArray{
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/d.cpp"_s}, {u"oldText"_s, u"p"_s}, {u"newText"_s, u"q"_s}},
            }},
        };
        const PermissionRequest r = client.permissionFromToolCall(call);
        QCOMPARE(r.path, u"/d.cpp"_s);
        QVERIFY(r.mutatesFiles);
        QVERIFY(r.describeDiff.contains(u"+q"_s));
    }

    void camelCaseKeys()
    {
        AcpClient client;
        const QJsonObject call{
            {u"toolCallId"_s, u"t5"_s},
            {u"kind"_s, u"edit"_s},
            {u"rawInput"_s, QJsonObject{{u"filePath"_s, u"/e.cpp"_s}, {u"oldText"_s, u"old"_s}, {u"newText"_s, u"new"_s}}},
        };
        const PermissionRequest r = client.permissionFromToolCall(call);
        QCOMPARE(r.path, u"/e.cpp"_s);
        QVERIFY(r.describeDiff.contains(u"+new"_s));
    }

    void terminalContentNotLeaked()
    {
        AcpClient client;
        const QJsonObject call{
            {u"toolCallId"_s, u"t6"_s},
            {u"kind"_s, u"execute"_s},
            {u"content"_s, QJsonArray{
                QJsonObject{{u"type"_s, u"terminal"_s}, {u"terminalId"_s, u"term_1"_s}},
            }},
        };
        const PermissionRequest r = client.permissionFromToolCall(call);
        QVERIFY(!r.details.contains(u"terminalId"_s));
    }

    void multipleDiffs()
    {
        AcpClient client;
        const QJsonObject call{
            {u"toolCallId"_s, u"t7"_s},
            {u"kind"_s, u"edit"_s},
            {u"content"_s, QJsonArray{
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/f.cpp"_s}, {u"oldText"_s, u"a"_s}, {u"newText"_s, u"b"_s}},
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/g.cpp"_s}, {u"oldText"_s, u"c"_s}, {u"newText"_s, u"d"_s}},
            }},
        };
        const PermissionRequest r = client.permissionFromToolCall(call);
        QCOMPARE(r.path, u"/f.cpp"_s);
        QCOMPARE(r.additionalPaths, QStringList{u"/g.cpp"_s});
        QCOMPARE(r.additionalDiffs.size(), 1);
    }

    void mergeToolCallDetails()
    {
        AcpClient client;
        const PermissionRequest a = client.permissionFromToolCall(QJsonObject{
            {u"toolCallId"_s, u"t8"_s},
            {u"kind"_s, u"edit"_s},
            {u"content"_s, QJsonArray{
                QJsonObject{{u"type"_s, u"diff"_s}, {u"path"_s, u"/h.cpp"_s}, {u"oldText"_s, u"x"_s}, {u"newText"_s, u"y"_s}},
            }},
        });
        const PermissionRequest b = client.permissionFromToolCall(QJsonObject{{u"toolCallId"_s, u"t8"_s}});
        QCOMPARE(b.toolName, u"tool"_s);
        const PermissionRequest merged = AcpClient::mergeToolCallDetails(a, b);
        QCOMPARE(merged.toolName, u"edit_file"_s);
        QCOMPARE(merged.path, u"/h.cpp"_s);
        QVERIFY(merged.describeDiff.contains(u"+y"_s));
    }
};

QTEST_GUILESS_MAIN(TestAcpParsing)
#include "test_acpparsing.moc"
