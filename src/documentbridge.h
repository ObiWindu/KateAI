/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>
#include <QObject>

namespace KateAi
{

class DocumentBridge : public QObject
{
    Q_OBJECT
public:
    explicit DocumentBridge(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~DocumentBridge() = default;
    virtual bool readDocument(const QString &path, QString *contents) const = 0;
    virtual bool writeDocument(const QString &path, const QString &contents, QString *error) = 0;
};

class DiskDocumentBridge : public DocumentBridge
{
    Q_OBJECT
public:
    explicit DiskDocumentBridge(QObject *parent = nullptr) : DocumentBridge(parent) {}
    bool readDocument(const QString &path, QString *contents) const override;
    bool writeDocument(const QString &path, const QString &contents, QString *error) override;
};

} // namespace KateAi