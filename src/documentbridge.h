/*
 * SPDX-FileCopyrightText: 2026 ObiWindu <Obi.wandu@proton.me>
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <QString>
#include <QStringList>

namespace KateAi
{

class DocumentBridge
{
public:
    virtual ~DocumentBridge() = default;
    virtual bool readDocument(const QString &path, QString *contents) const = 0;
    virtual bool readDocumentLines(const QString &path, int startLine, int maxLines,
                                   QStringList *page, int *totalLines) const;
    virtual bool writeDocument(const QString &path, const QString &contents, QString *error) = 0;
};

class DiskDocumentBridge : public DocumentBridge
{
public:
    bool readDocument(const QString &path, QString *contents) const override;
    bool readDocumentLines(const QString &path, int startLine, int maxLines,
                           QStringList *page, int *totalLines) const override;
    bool writeDocument(const QString &path, const QString &contents, QString *error) override;
};

} // namespace KateAi
