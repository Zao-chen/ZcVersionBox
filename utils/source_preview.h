#pragma once
#include "backup_types.h"
#include <QByteArray>
#include <QImage>

namespace SourcePreview
{
constexpr qint64 TextLimit = 2 * 1024 * 1024;
constexpr int LineLimit = 20000;
constexpr qint64 ImageLimit = 20 * 1024 * 1024;
constexpr qint64 PixelLimit = 20000000;
}
struct SourceLocation
{
    BranchContext context;
    QString root, path;
    bool rootDirectory{false};
    QByteArray rootIdentity;
};
struct SourceEntry
{
    QString path, name;
    bool directory{false}, link{false};
    qint64 size{0};
    QDateTime modified;
};
struct SourceDirectory
{
    SourceLocation location;
    SourceEntry entry;
    QVector<SourceEntry> children;
};
struct SourceContent
{
    SourceLocation location;
    QByteArray bytes, stamp;
    QString text, encoding{"UTF-8"}, newline{"\n"}, notice;
    bool bom{false}, textual{false}, editable{false};
    QImage image;
};
struct SourceChanges
{
    QString head;
    QMap<QString, Revision> revisions;
};
// Shared bounded decoding also keeps Markdown resources within the same limits.
SourceContent decodeSourceContent(const QByteArray &bytes);
BackupResult<QString> sourcePreviewPath(const SourceLocation &location, bool mustExist = true);
BackupResult<QByteArray> sourcePreviewStamp(const QString &path);

struct SourceSaveRequest
{
    SourceLocation location;
    QByteArray expectedStamp;
    QString text;
    bool create{false};
};
enum class SourceMutationKind { CreateDirectory, Rename, Delete };
struct SourceMutationRequest
{
    SourceLocation location;
    SourceMutationKind kind{SourceMutationKind::Delete};
    QString name;
    QMap<QString, QByteArray> snapshot;
};
QByteArray sourcePreviewIdentity(const QString &path);
bool validSourceEntryName(const QString &name);
BackupResult<QByteArray> encodeSourceText(const SourceContent &original, const QString &text);
