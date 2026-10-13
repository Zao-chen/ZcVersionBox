#include "backup_engine.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimeZone>
#include <algorithm>

namespace
{
SourceLocation location(const BackupRecord &record, const QString &path)
{
    return {{record.id, record.branchRef, record.lastCommit, record.generation, record.branchVersion}, record.sourcePath, path, record.directory};
}
SourceEntry entry(const QString &absolute, const QString &relative)
{
    QFileInfo info(absolute);
    return {relative, info.fileName(), info.isDir(), BackupFiles::isLink(absolute), info.size(), info.lastModified()};
}
}
BackupResult<SourceDirectory> BackupEngine::sourceDirectory(const QString &id, const QString &path)
{
    const auto *record = m_catalog.find(id);
    if (!record) return {OperationResult::fail("对象不存在", {})};
    SourceDirectory result;
    result.location = location(*record, path);
    const auto resolved = sourcePreviewPath(result.location);
    if (!resolved.result.success) return {resolved.result};
    result.entry = entry(resolved.value, path);
    if (result.entry.directory)
    {
        const auto children = m_dependencies.files->children(resolved.value);
        if (!children.result.success) return {children.result};
        for (const auto &child : children.value)
        {
            const auto name = QFileInfo(child).fileName();
            if (!BackupSelectionPolicy::copies(name)) continue;
            result.children.append(entry(child, path.isEmpty() ? name : path + '/' + name));
        }
        std::sort(result.children.begin(), result.children.end(), [](const SourceEntry &a, const SourceEntry &b) {
            if (a.directory != b.directory) return a.directory;
            return QString::localeAwareCompare(a.name, b.name) < 0;
        });
    }
    return {OperationResult::ok({}), result};
}
BackupResult<SourceContent> BackupEngine::sourceContent(const QString &id, const QString &path)
{
    const auto *record = m_catalog.find(id);
    if (!record) return {OperationResult::fail("对象不存在", {})};
    const auto where = location(*record, path);
    const auto resolved = sourcePreviewPath(where);
    if (!resolved.result.success) return {resolved.result};
    if (!QFileInfo(resolved.value).isFile()) return {OperationResult::fail("不是普通文件", {})};
    SourceContent content;
    content.location = where;
    if (QFileInfo(resolved.value).size() > SourcePreview::ImageLimit)
    { content.notice = "文件超出预览上限，请用系统应用打开。"; return {OperationResult::ok({}), content}; }
    auto stamp = sourcePreviewStamp(resolved.value);
    if (!stamp.result.success) return {stamp.result};
    QFile file(resolved.value);
    if (!BackupFiles::openForFingerprint(file)) return {OperationResult::fail("无法读取文件", file.errorString())};
    auto bytes = file.read(SourcePreview::ImageLimit + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > SourcePreview::ImageLimit) return {OperationResult::fail("文件正在变化或无法读取", {})};
    file.close();
    const auto after = sourcePreviewStamp(resolved.value);
    if (!after.result.success || after.value != stamp.value) return {OperationResult::fail("文件已变化", "请重新读取。")};
    content = decodeSourceContent(bytes);
    content.location = where;
    content.stamp = stamp.value;
    return {OperationResult::ok({}), content};
}
BackupResult<SourceChanges> BackupEngine::sourceChanges(const QString &id, const QStringList &paths)
{
    const auto record = require(id);
    if (!record.result.success) return {record.result};
    if (paths.size() > 64) return {OperationResult::fail("查询过大", "每批最多查询 64 个路径。")};
    auto git = repository(id);
    const auto head = git.head();
    if (!head.result.success) return {head.result};
    SourceChanges result; result.head = head.value;
    for (const auto &path : paths)
    {
        const auto resolved = sourcePreviewPath(location(record.value, path));
        if (!resolved.result.success) continue;
        const auto key = id + ':' + head.value + ':' + record.value.repositoryPath + ':' + path;
        if (m_sourceChanges.contains(key)) { result.revisions.insert(path, m_sourceChanges.value(key)); continue; }
        const auto mapped = path.isEmpty() ? record.value.repositoryPath :
            (record.value.repositoryPath == "." ? path : record.value.repositoryPath + '/' + path);
        auto log = git.run({"log", "-1", "--format=%H%x00%ct%x00%s", head.value, "--", mapped});
        if (!log.success()) return {GitRepository::outcome(log)};
        Revision revision;
        if (!log.output.isEmpty())
        {
            const auto fields = log.output.trimmed().split(QChar(0));
            if (fields.size() != 3) return {OperationResult::fail("提交信息无法解析", {})};
            revision.hash = fields[0]; revision.message = fields[2];
            revision.committedAt = QDateTime::fromSecsSinceEpoch(fields[1].toLongLong(), QTimeZone::UTC);
        }
        if (m_sourceChanges.size() >= 4096) m_sourceChanges.clear();
        m_sourceChanges.insert(key, revision);
        result.revisions.insert(path, revision);
    }
    return {OperationResult::ok({}), result};
}
