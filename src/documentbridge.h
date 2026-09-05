#pragma once

#include <QString>

namespace KateAi
{

class DocumentBridge
{
public:
    virtual ~DocumentBridge() = default;
    virtual bool readDocument(const QString &path, QString *contents) const = 0;
    virtual bool writeDocument(const QString &path, const QString &contents, QString *error) = 0;
};

class DiskDocumentBridge : public DocumentBridge
{
public:
    bool readDocument(const QString &path, QString *contents) const override;
    bool writeDocument(const QString &path, const QString &contents, QString *error) override;
};

} // namespace KateAi
