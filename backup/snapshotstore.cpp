#include "snapshotstore.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

namespace Backup
{
namespace
{
const QString notesRef = "refs/notes/zcversionbox";
QByteArray quoted(const QString &path)
{
    QByteArray result("\"");
    for (unsigned char c : path.toUtf8())
    {
        if (c == '\\' || c == '"')
        {
            result += '\\';
            result += char(c);
        }
        else if (c < 32 || c >= 127)
            result += '\\' + QByteArray::number(c, 8).rightJustified(3, '0');
        else
            result += char(c);
    }
    return result + '"';
}
void validRevision(const QString &revision)
{
    static const QRegularExpression pattern("^[0-9a-f]{40}$");
    if (!pattern.match(revision).hasMatch())
        throw Error(ErrorCode::InvalidFormat, "必须使用完整快照 ID");
}
QByteArray oidFor(const QByteArray &bytes)
{
    return QCryptographicHash::hash("blob " + QByteArray::number(bytes.size()) + '\0' + bytes, QCryptographicHash::Sha1).toHex();
}
QJsonObject parseNote(const QByteArray &bytes)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    const auto note = document.object();
    if (error.error != QJsonParseError::NoError || !document.isObject() || note["version"].toInt() != 1 ||
        (note.contains("manual") && !note["manual"].isString()) || (note.contains("ai") && !note["ai"].isString()))
        throw Error(ErrorCode::InvalidFormat, "不支持或损坏的快照备注格式");
    return note;
}
} // namespace
SnapshotStore::SnapshotStore(QString path, Cancellation cancel) : m_path(std::move(path)), m_cancel(std::move(cancel)) {}
void SnapshotStore::initialize()
{
    git({}, {"init", "--bare", "--initial-branch=main", "--object-format=sha1", m_path}, {}, m_cancel);
    git(m_path, {"config", "user.name", "ZcVersionBox"}, {}, m_cancel);
    git(m_path, {"config", "user.email", "backup@zcversionbox.local"}, {}, m_cancel);
    git(m_path, {"config", "gc.auto", "0"}, {}, m_cancel);
}
QString SnapshotStore::head(const QString &ref) const
{
    GitProcess p(m_path, {"rev-parse", "--verify", "--quiet", ref}, m_cancel);
    p.closeInput();
    const auto output = p.finish({0, 1});
    return p.exitCode() == 0 ? QString::fromLatin1(output).trimmed() : QString();
}
Snapshot SnapshotStore::readSnapshot(const QString &revision) const
{
    validRevision(revision);
    QByteArray raw;
    try
    {
        raw = git(m_path, {"cat-file", "blob", revision + ":.zcversionbox.json"}, {}, m_cancel);
    }
    catch (const Error &e)
    {
        if (e.code == ErrorCode::Git)
            throw Error(ErrorCode::InvalidFormat, "缺少新版快照清单，仅支持新版 ZcVersionBox 仓库");
        throw;
    }
    const auto document = QJsonDocument::fromJson(raw);
    if (!document.isObject())
        throw Error(ErrorCode::InvalidFormat, "缺少新版快照清单");
    const auto snapshot = Snapshot::parse(document.object());
    const auto tree = git(m_path, {"ls-tree", "-r", "-z", "--full-tree", revision}, {}, m_cancel);
    int count = 0;
    for (const auto &entry : tree.split('\0'))
    {
        if (entry.isEmpty())
            continue;
        const auto tab = entry.indexOf('\t');
        const auto fields = entry.left(tab).split(' ');
        const auto name = QString::fromUtf8(entry.mid(tab + 1));
        if (fields.size() != 3 || fields[1] != "blob")
            throw Error(ErrorCode::InvalidFormat, "快照对象类型无效");
        if (name == ".zcversionbox.json")
        {
            if (fields[0] != "100644" || fields[2] != oidFor(raw))
                throw Error(ErrorCode::InvalidFormat, "快照清单对象无效");
            continue;
        }
        if (!name.startsWith("payload/"))
            throw Error(ErrorCode::InvalidFormat, "仓库包含未知内容");
        const auto f = snapshot.files.constFind(name.mid(8));
        if (f == snapshot.files.cend() || f->oid.toLatin1() != fields[2] || fields[0] != (f->executable ? "100755" : "100644"))
            throw Error(ErrorCode::InvalidFormat, "快照清单与 Git 内容不一致");
        ++count;
    }
    if (count != snapshot.files.size())
        throw Error(ErrorCode::InvalidFormat, "快照缺少文件对象");
    return snapshot;
}
QString SnapshotStore::candidate(const Snapshot &s, const QString &source, const QString &base, const QString &pending, const QString &operation, TaskResult *stats) const
{
    Snapshot previous;
    if (!base.isEmpty())
        previous = readSnapshot(base);
    GitProcess process(m_path, {"fast-import", "--quiet", "--done"}, m_cancel);
    const auto message = (operation + " · " + QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)).toUtf8();
    process.write("commit " + pending.toUtf8() + "\ncommitter ZcVersionBox <backup@zcversionbox.local> " + QByteArray::number(QDateTime::currentSecsSinceEpoch()) + " +0000\ndata " + QByteArray::number(message.size()) + '\n' + message + '\n');
    if (!base.isEmpty())
        process.write("from " + base.toLatin1() + '\n');
    for (auto it = previous.files.cbegin(); it != previous.files.cend(); ++it)
        if (!s.files.contains(it.key()))
            process.write("D " + quoted("payload/" + it.key()) + '\n');
    for (auto it = s.files.cbegin(); it != s.files.cend(); ++it)
    {
        checkCancelled(m_cancel);
        const auto prior = previous.files.constFind(it.key());
        if (prior != previous.files.cend() && prior->oid == it->oid && prior->executable == it->executable)
            continue;
        const auto path = s.kind == "file" ? source : QDir(source).filePath(it.key());
        const auto identity = fileIdentityWithin(source, path);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw Error(ErrorCode::Permission, "无法读取：" + path);
        if (file.size() != it->size)
            throw Error(ErrorCode::SourceChanged, "源文件已变化", true);
        process.write(QByteArray("M ") + (it->executable ? "100755" : "100644") + " inline " + quoted("payload/" + it.key()) + "\ndata " + QByteArray::number(it->size) + '\n');
        QCryptographicHash hash(QCryptographicHash::Sha1);
        hash.addData("blob " + QByteArray::number(it->size) + '\0');
        qint64 total = 0;
        while (total < it->size)
        {
            checkCancelled(m_cancel);
            const auto chunk = file.read(qMin<qint64>(1024 * 1024, it->size - total));
            if (chunk.isEmpty())
                throw Error(ErrorCode::SourceChanged, "源文件读取中断", true);
            process.write(chunk);
            hash.addData(chunk);
            total += chunk.size();
        }
        if (identity != fileIdentityWithin(source, path) || QString::fromLatin1(hash.result().toHex()) != it->oid)
            throw Error(ErrorCode::SourceChanged, "源文件在备份期间发生变化", true);
        process.write("\n");
        if (stats)
            stats->bytesWritten += total;
    }
    const auto manifest = QJsonDocument(s.json()).toJson(QJsonDocument::Compact);
    process.write("M 100644 inline .zcversionbox.json\ndata " + QByteArray::number(manifest.size()) + '\n' + manifest + "\n\ndone\n");
    process.closeInput();
    process.finish();
    const auto revision = head(pending);
    if (revision.isEmpty())
        throw Error(ErrorCode::Git, "候选快照未生成");
    readSnapshot(revision);
    return revision;
}
QString SnapshotStore::copyCommit(const QString &revision, const QString &base, const QString &operation) const
{
    readSnapshot(revision);
    const auto tree = QString::fromLatin1(git(m_path, {"rev-parse", revision + "^{tree}"}, {}, m_cancel)).trimmed();
    QStringList args{"commit-tree", tree};
    if (!base.isEmpty())
        args << "-p" << base;
    const auto message = operation + " · " + QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + "\n来源版本：" + revision + '\n';
    return QString::fromLatin1(git(m_path, args, message.toUtf8(), m_cancel)).trimmed();
}
void SnapshotStore::publish(const QString &revision, const QString &base) const
{
    validRevision(revision);
    try
    {
        git(m_path, {"update-ref", "refs/heads/main", revision, base.isEmpty() ? QString(40, '0') : base}, {}, m_cancel);
    }
    catch (const Error &e)
    {
        // Cancellation/timeout may arrive after Git has already changed the ref.
        const auto current = SnapshotStore(m_path).head();
        if (current == revision)
            return;
        if (current != base)
            throw Error(ErrorCode::Conflict, "仓库引用已被其他操作改变");
        throw;
    }
}
void SnapshotStore::exportSnapshot(const QString &revision, const QString &destination, bool readOnly) const
{
    const auto s = readSnapshot(revision);
    if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())
        throw Error(ErrorCode::Io, "导出目标必须不存在");
    QSet<QString> allNames;
    for (const auto &name : s.directories + s.files.keys())
    {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        const auto key = name.normalized(QString::NormalizationForm_C).toCaseFolded();
#else
        const auto key = name;
#endif
        if (allNames.contains(key))
            throw Error(ErrorCode::Unsupported, "目标文件系统存在名称冲突：" + name);
        allNames.insert(key);
    }
    if (!QDir().mkpath(s.kind == "directory" ? destination : QFileInfo(destination).absolutePath()))
        throw Error(ErrorCode::Permission, "无法创建导出目录");
    for (const auto &dir : s.directories)
        if (!QDir().mkpath(QDir(destination).filePath(dir)))
            throw Error(ErrorCode::Io, "无法创建快照目录", true);
    GitProcess batch(m_path, {"cat-file", "--batch"}, m_cancel);
    QSet<QString> names;
    for (auto it = s.files.cbegin(); it != s.files.cend(); ++it)
    {
        checkCancelled(m_cancel);
        const auto path = s.kind == "file" ? destination : QDir(destination).filePath(it.key());
        // Case folding catches aliases before an existing file can be replaced on Windows/macOS.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        const auto key = path.normalized(QString::NormalizationForm_C).toCaseFolded();
#else
        const auto key = path;
#endif
        if (names.contains(key) || QFileInfo::exists(path))
            throw Error(ErrorCode::InvalidFormat, "目标文件系统存在名称冲突：" + it.key());
        names.insert(key);
        batch.write(it->oid.toLatin1() + '\n');
        const auto header = batch.readLine().trimmed().split(' ');
        if (header.size() != 3 || header[0] != it->oid.toLatin1() || header[1] != "blob" || header[2].toLongLong() != it->size)
            throw Error(ErrorCode::InvalidFormat, "快照对象长度错误");
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            throw Error(ErrorCode::Permission, "无法写入：" + path);
        qint64 remaining = it->size;
        QCryptographicHash hash(QCryptographicHash::Sha1);
        hash.addData("blob " + QByteArray::number(it->size) + '\0');
        while (remaining)
        {
            const auto chunk = batch.read(qMin<qint64>(remaining, 1024 * 1024));
            hash.addData(chunk);
            if (file.write(chunk) != chunk.size())
                throw Error(ErrorCode::Io, "快照写入失败", true);
            remaining -= chunk.size();
        }
        if (batch.read(1) != "\n" || QString::fromLatin1(hash.result().toHex()) != it->oid)
            throw Error(ErrorCode::InvalidFormat, "快照对象校验失败");
        if (!file.commit())
            throw Error(ErrorCode::Io, "快照文件提交失败", true);
        auto permissions = QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther;
        if (!readOnly)
            permissions |= QFile::WriteOwner | QFile::WriteUser;
        if (it->executable)
            permissions |= QFile::ExeOwner | QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther;
        if (!QFile::setPermissions(path, permissions))
            throw Error(ErrorCode::Permission, "无法设置文件权限：" + path);
    }
    batch.closeInput();
    batch.finish();
    if (readOnly && s.kind == "directory")
    {
        const auto permissions = QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther |
                                 QFile::ExeOwner | QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther;
        auto directories = s.directories;
        directories.append(QString());
        for (const auto &directory : directories)
        {
            const auto path = directory.isEmpty() ? destination : QDir(destination).filePath(directory);
            if (!QFile::setPermissions(path, permissions))
                throw Error(ErrorCode::Permission, "无法将预览目录设为只读：" + path);
        }
    }
}
QJsonObject SnapshotStore::history(int offset, int limit) const
{
    limit = qBound(1, limit, 200);
    QJsonArray rows;
    if (head().isEmpty())
        return {{"rows", rows}, {"hasMore", false}};
    const auto bytes = git(m_path, {"log", "refs/heads/main", "--format=%H%x00%ct%x00%s%x00", "--skip=" + QString::number(qMax(0, offset)), "-n", QString::number(qBound(1, limit, 200) + 1)}, {}, m_cancel);
    const auto fields = bytes.split('\0');
    for (int i = 0; i + 2 < fields.size(); i += 3)
    {
        const auto id = QString::fromLatin1(fields[i]).trimmed();
        if (id.isEmpty())
            continue;
        const auto note = git(m_path, {"notes", "--ref=zcversionbox", "show", id}, {}, m_cancel, {0, 1});
        rows.append(QJsonObject{{"id", id}, {"time", QString::fromLatin1(fields[i + 1])}, {"summary", QString::fromUtf8(fields[i + 2])}, {"annotation", note.isEmpty() ? QJsonObject() : parseNote(note)}});
    }
    bool more = rows.size() > limit;
    if (more)
        rows.removeLast();
    return {{"rows", rows}, {"hasMore", more}};
}
QJsonObject SnapshotStore::diff(const QString &revision) const
{
    readSnapshot(revision);
    const auto output = git(m_path, {"show", "--format=", "--root", "--no-ext-diff", "--no-textconv", "--stat", "--patch", revision, "--", "payload"}, {}, m_cancel);
    return {{"text", QString::fromUtf8(output)}};
}
bool SnapshotStore::ancestor(const QString &older, const QString &newer) const
{
    if (older.isEmpty())
        return true;
    GitProcess p(m_path, {"merge-base", "--is-ancestor", older, newer}, m_cancel);
    p.closeInput();
    p.finish({0, 1});
    return p.exitCode() == 0;
}
QString SnapshotStore::fetch(const QString &remote) const
{
    if (remote.isEmpty() || remote.startsWith('-'))
        throw Error(ErrorCode::InvalidFormat, "远端地址无效");
    git(m_path, {"fetch", "--prune", "--no-tags", "--", remote, "+refs/heads/main:refs/remotes/cloud/main", "+refs/notes/*:refs/remotes/cloud-notes/*"}, {}, m_cancel, {0}, 300000);
    const auto revision = head("refs/remotes/cloud/main");
    readSnapshot(revision);
    validateNotes("refs/remotes/cloud-notes/zcversionbox");
    return revision;
}
void SnapshotStore::push(const QString &remote, const QString &lease) const
{
    if (remote.isEmpty() || remote.startsWith('-'))
        throw Error(ErrorCode::InvalidFormat, "远端地址无效");
    QStringList args{"push", "--atomic"};
    if (!lease.isEmpty())
    {
        validRevision(lease);
        args << "--force-with-lease=refs/heads/main:" + lease;
    }
    args << "--" << remote << "refs/heads/main:refs/heads/main";
    if (!head(notesRef).isEmpty())
        args << notesRef + ':' + notesRef;
    try
    {
        git(m_path, args, {}, m_cancel, {0}, 300000);
    }
    catch (const Error &e)
    {
        if (e.message.contains("[rejected]") || e.message.contains("non-fast-forward") || e.message.contains("stale info"))
            throw Error(ErrorCode::Conflict, "远端已变化或历史分叉：\n" + e.message);
        throw;
    }
}
void SnapshotStore::annotate(const QString &revision, const QString &text, bool ai) const
{
    readSnapshot(revision);
    const auto bytes = git(m_path, {"notes", "--ref=zcversionbox", "show", revision}, {}, m_cancel, {0, 1});
    auto annotation = bytes.isEmpty() ? QJsonObject() : parseNote(bytes);
    annotation["version"] = 1;
    annotation[ai ? "ai" : "manual"] = text;
    const auto encoded = QJsonDocument(annotation).toJson(QJsonDocument::Compact);
    if (encoded.size() > 1024 * 1024)
        throw Error(ErrorCode::Unsupported, "备注不能超过 1 MiB");
    git(m_path, {"notes", "--ref=zcversionbox", "add", "-f", "-F", "-", revision}, encoded, m_cancel);
}
void SnapshotStore::validateNotes(const QString &ref) const
{
    if (head(ref).isEmpty())
        return;
    const auto entries = git(m_path, {"ls-tree", "-r", "-z", ref}, {}, m_cancel);
    GitProcess batch(m_path, {"cat-file", "--batch"}, m_cancel);
    for (const auto &entry : entries.split('\0'))
    {
        if (entry.isEmpty())
            continue;
        const auto tab = entry.indexOf('\t');
        const auto fields = entry.left(tab).split(' ');
        if (tab < 0 || fields.size() != 3 || fields[0] != "100644" || fields[1] != "blob")
            throw Error(ErrorCode::InvalidFormat, "远端备注树结构无效");
        const auto oid = fields[2];
        validRevision(QString::fromLatin1(entry.mid(tab + 1)).remove('/'));
        validRevision(QString::fromLatin1(oid));
        batch.write(oid + '\n');
        const auto header = batch.readLine().trimmed().split(' ');
        bool validSize = false;
        const auto size = header.size() == 3 ? header[2].toLongLong(&validSize) : -1;
        if (!validSize || size < 0 || size > 1024 * 1024 || header[0] != oid || header[1] != "blob")
            throw Error(ErrorCode::InvalidFormat, "远端备注对象无效或超过 1 MiB");
        QByteArray bytes;
        while (bytes.size() < size)
            bytes += batch.read(size - bytes.size());
        if (batch.read(1) != "\n")
            throw Error(ErrorCode::InvalidFormat, "远端备注对象不完整");
        parseNote(bytes);
    }
    batch.closeInput();
    batch.finish();
}
void SnapshotStore::syncNotes() const
{
    const auto remote = head("refs/remotes/cloud-notes/zcversionbox"), local = head(notesRef);
    if (remote.isEmpty() || remote == local || (!local.isEmpty() && ancestor(remote, local)))
        return;
    if (!local.isEmpty() && !ancestor(local, remote))
        throw Error(ErrorCode::Conflict, "备注历史已分叉，需要解决冲突");
    git(m_path, {"update-ref", notesRef, remote, local.isEmpty() ? QString(40, '0') : local}, {}, m_cancel);
}
} // namespace Backup
