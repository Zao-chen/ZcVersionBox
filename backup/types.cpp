#include "types.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace Backup
{
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString storageRoot() { return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("backup-engine"); }
QString normalizedPath(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
bool containsPath(const QString &parent, const QString &child)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    const auto p = normalizedPath(parent), c = normalizedPath(child);
    return p.compare(c, sensitivity) == 0 || c.startsWith(p.endsWith('/') ? p : p + '/', sensitivity);
}
void validateRelative(const QString &path)
{
    if (path.isEmpty() || path.contains(QChar(0)) || path.startsWith('/'))
        throw Error(ErrorCode::InvalidFormat, "快照中存在不安全路径：" + path);
#ifdef Q_OS_WIN
    if (path.contains('\\'))
        throw Error(ErrorCode::InvalidFormat, "快照中存在不安全路径：" + path);
#endif
    for (const auto &part : path.split('/'))
    {
        if (part.isEmpty() || part == "." || part == ".." || part.compare(".git", Qt::CaseInsensitive) == 0)
            throw Error(ErrorCode::InvalidFormat, "快照中存在不安全路径：" + path);
#ifdef Q_OS_WIN
        static const QRegularExpression reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption);
        if (part.contains(QRegularExpression("[<>:\"|?*]")) || part.endsWith('.') || part.endsWith(' ') || reserved.match(part).hasMatch())
            throw Error(ErrorCode::Unsupported, "当前文件系统不支持文件名：" + path);
#endif
    }
}
QJsonObject Tracking::json() const { return {{"format", 1}, {"id", id}, {"source", source}, {"remote", remote}, {"pendingImport", pendingImport}}; }
Tracking Tracking::parse(const QJsonObject &v)
{
    Tracking t{v["id"].toString(), v["source"].toString(), v["remote"].toString(), v["pendingImport"].toBool()};
    if (v["format"].toInt() != 1 || QUuid(t.id).isNull() || !QDir::isAbsolutePath(t.source) || !v["pendingImport"].isBool())
        throw Error(ErrorCode::InvalidFormat, "不支持的备份配置格式");
    return t;
}
bool Snapshot::sameContent(const Snapshot &o) const
{
    if (kind != o.kind || directories != o.directories || files.size() != o.files.size())
        return false;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
    {
        auto other = o.files.constFind(it.key());
        if (other == o.files.cend() || it->oid != other->oid || it->executable != other->executable)
            return false;
    }
    return true;
}
QJsonObject Snapshot::json() const
{
    QJsonObject entries;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        entries[it.key()] = QJsonObject{{"oid", it->oid}, {"size", QString::number(it->size)}, {"executable", it->executable}};
    return {{"format", "zcversionbox-snapshot"}, {"version", 1}, {"kind", kind}, {"name", name}, {"files", entries}, {"directories", QJsonArray::fromStringList(directories)}};
}
Snapshot Snapshot::parse(const QJsonObject &v)
{
    if (v["format"] != "zcversionbox-snapshot" || v["version"].toInt() != 1 || !v["files"].isObject() || !v["directories"].isArray())
        throw Error(ErrorCode::InvalidFormat, "仅支持新版 ZcVersionBox 快照仓库");
    Snapshot s;
    s.kind = v["kind"].toString();
    s.name = v["name"].toString();
    if ((s.kind != "file" && s.kind != "directory") || s.name.isEmpty())
        throw Error(ErrorCode::InvalidFormat, "快照根对象无效");
    validateRelative(s.name);
    if (s.name.contains('/'))
        throw Error(ErrorCode::InvalidFormat, "快照根名称不能包含路径");
    const auto files = v["files"].toObject();
    static const QRegularExpression oid("^[0-9a-f]{40}$");
    for (auto it = files.begin(); it != files.end(); ++it)
    {
        validateRelative(it.key());
        auto e = it->toObject();
        bool ok = false;
        FileEntry f{e["oid"].toString(), e["size"].toString().toLongLong(&ok), e["executable"].toBool()};
        if (!ok || f.size < 0 || !oid.match(f.oid).hasMatch())
            throw Error(ErrorCode::InvalidFormat, "快照文件记录无效");
        s.files.insert(it.key(), f);
    }
    for (const auto &dir : v["directories"].toArray())
    {
        validateRelative(dir.toString());
        if (s.files.contains(dir.toString()))
            throw Error(ErrorCode::InvalidFormat, "快照路径类型冲突");
        s.directories.append(dir.toString());
    }
    s.directories.removeDuplicates();
    s.directories.sort();
    if (s.kind == "file" && (s.files.size() != 1 || !s.files.contains("file") || !s.directories.isEmpty()))
        throw Error(ErrorCode::InvalidFormat, "单文件快照结构无效");
    for (const auto &path : s.files.keys() + s.directories)
    {
        QString p = path;
        while (p.contains('/'))
        {
            p = p.left(p.lastIndexOf('/'));
            if (s.files.contains(p) || !s.directories.contains(p))
                throw Error(ErrorCode::InvalidFormat, "快照目录结构无效");
        }
    }
    return s;
}
QJsonObject ConflictInfo::json() const { return {{"operation", operation}, {"category", category}, {"base", base}, {"local", local}, {"remote", remote}, {"paths", QJsonArray::fromStringList(paths)}}; }
void writeJson(const QString &path, const QJsonObject &value)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        throw Error(ErrorCode::Permission, "无法创建数据目录");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        throw Error(ErrorCode::Permission, file.errorString());
    const auto bytes = QJsonDocument(value).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit())
        throw Error(ErrorCode::Io, file.errorString(), true);
}
QJsonObject readJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw Error(ErrorCode::Io, "无法读取：" + path);
    QJsonParseError e;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &e);
    if (e.error != QJsonParseError::NoError || !doc.isObject())
        throw Error(ErrorCode::InvalidFormat, "数据文件损坏：" + path);
    return doc.object();
}
void removeOwnedPath(const QString &path)
{
    QFileInfo f(path);
    if (!f.exists() && !f.isSymLink())
        return;
    // Export directories are deliberately read-only. Unlock owned directories before removing their children.
    if (f.isDir() && !f.isSymLink())
    {
        if (!(f.permissions() & QFile::WriteOwner) && !QFile::setPermissions(path, f.permissions() | QFile::WriteOwner))
            throw Error(ErrorCode::Permission, "无法清理只读目录：" + path);
        for (const auto &entry : QDir(path).entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
            removeOwnedPath(entry.absoluteFilePath());
    }
    bool ok = f.isDir() && !f.isSymLink() ? QDir(path).removeRecursively() : QFile::remove(path);
    if (!ok)
        throw Error(ErrorCode::Io, "无法清理：" + path, true);
}
void renamePath(const QString &from, const QString &to)
{
    if (QFileInfo::exists(to) || QFileInfo(to).isSymLink() || !QDir().rename(from, to))
        throw Error(ErrorCode::Io, "无法重命名：" + from + " → " + to, true);
}
} // namespace Backup
