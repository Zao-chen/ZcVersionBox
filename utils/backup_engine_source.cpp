#include "backup_engine.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimeZone>
#include <QSaveFile>
#include <QTemporaryFile>
#include <algorithm>

namespace
{
SourceLocation location(const BackupRecord &record, const QString &path)
{
    return {{record.id, record.branchRef, record.lastCommit, record.generation, record.branchVersion}, record.sourcePath, path, record.directory, sourcePreviewIdentity(record.sourcePath)};
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

BackupResult<BackupRecord> BackupEngine::sourceRecord(const SourceLocation &where)
{
    const auto record = require(where.context.id, true);
    if (!record.result.success) return record;
    const auto &r = record.value;
    if (r.sourcePath != where.root || r.directory != where.rootDirectory || r.generation != where.context.generation ||
        r.branchVersion != where.context.version || r.branchRef != where.context.ref || where.rootIdentity.isEmpty() ||
        sourcePreviewIdentity(r.sourcePath) != where.rootIdentity)
        return {OperationResult::fail("源内容或方案已变化", "请刷新后再操作；编辑内容仍可保留。")};
    return record;
}
BackupResult<QMap<QString, QByteArray>> BackupEngine::sourceSnapshot(const QString &root)
{
    QMap<QString, QByteArray> snapshot;
    std::function<OperationResult(const QString &, const QString &)> visit = [&](const QString &path, const QString &relative) {
        if (m_cancel && m_cancel->load()) return OperationResult::cancel("操作已取消");
        if (BackupFiles::hasLinkedAncestor(path) || BackupFiles::isGitMetadataPath(path))
            return OperationResult::fail("操作已停止", "目标包含 .git 或链接，不能重命名或删除：" + path);
        const QFileInfo info(path);
        if (info.isDir())
        {
            auto children = m_dependencies.files->children(path);
            if (!children.result.success) return children.result;
            const auto identity = sourcePreviewIdentity(path);
            if (identity.isEmpty()) return OperationResult::fail("无法确认目录身份", path);
            snapshot.insert(relative + '/', identity + ':' + QByteArray::number(info.permissions()));
            for (const auto &child : children.value)
            {
                const auto result = visit(child, relative + '/' + QFileInfo(child).fileName());
                if (!result.success) return result;
            }
        }
        else
        {
            const auto stamp = sourcePreviewStamp(path);
            if (!stamp.result.success) return stamp.result;
            snapshot.insert(relative, stamp.value);
        }
        return OperationResult::ok({});
    };
    const auto result = visit(root, {});
    return {result, snapshot};
}

OperationResult BackupEngine::saveSourceFile(const SourceSaveRequest &request)
{
    const auto record = sourceRecord(request.location);
    if (!record.result.success) return record.result;
    const auto resolved = sourcePreviewPath(request.location, !request.create);
    if (!resolved.result.success) return resolved.result;
    const auto path = resolved.value;
    SourceContent original;
    if (request.create)
    {
        if (!request.location.rootDirectory || request.location.path.isEmpty() || !validSourceEntryName(QFileInfo(path).fileName()) ||
            BackupFiles::exists(path) || !QFileInfo(QFileInfo(path).absolutePath()).isDir())
            return OperationResult::fail("无法新建文件", "名称无效、同名项目已存在，或父目录不存在。");
        original.editable = true;
    }
    else
    {
        const auto loaded = sourceContent(request.location.context.id, request.location.path);
        if (!loaded.result.success) return loaded.result;
        original = loaded.value;
        if (request.expectedStamp.isEmpty() || request.expectedStamp != original.stamp)
            return OperationResult::fail("文件已被修改", "没有覆盖外部修改。请复制保留编辑内容，重新加载后再处理。");
        if (!original.editable) return OperationResult::fail("此文件不可编辑", original.notice);
        if (original.text == request.text) return OperationResult::ok("已保存到本地");
        if (!QFileInfo(path).isWritable()) return OperationResult::fail("文件不可写", path);
    }
    const auto encoded = encodeSourceText(original, request.text);
    if (!encoded.result.success) return encoded.result;
    if (m_cancel && m_cancel->load()) return OperationResult::cancel("操作已取消");
    if (request.create)
    {
        const auto parent = QFileInfo(path).absolutePath();
        const auto parentIdentity = sourcePreviewIdentity(parent);
        QTemporaryFile file(QDir(parent).filePath(".zc-preview-XXXXXX"));
        if (parentIdentity.isEmpty() || !file.open()) return OperationResult::fail("无法准备新文件", file.errorString());
        const auto temporaryIdentity = sourcePreviewIdentity(file.fileName());
        if (file.write(encoded.value) != encoded.value.size() || !file.flush())
            return OperationResult::fail("无法完整写入新文件", file.errorString());
        file.close();
        if (!sourcePreviewPath(request.location, false).result.success || BackupFiles::exists(path) ||
            sourcePreviewIdentity(parent) != parentIdentity || sourcePreviewIdentity(file.fileName()) != temporaryIdentity ||
            sourcePreviewIdentity(request.location.root) != request.location.rootIdentity)
            return OperationResult::fail("目标已变化", "没有创建或覆盖文件，请重新读取目录。");
        if (m_cancel && m_cancel->load()) return OperationResult::cancel("操作已取消");
        // QTemporaryFile renames within the same directory and refuses an existing target.
        if (!file.rename(path)) return OperationResult::fail("无法创建文件", file.errorString());
        file.setAutoRemove(false);
        return OperationResult::ok("已保存到本地");
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return OperationResult::fail("无法保存文件", file.errorString());
    if (!file.setPermissions(QFileInfo(path).permissions()) || file.write(encoded.value) != encoded.value.size())
        return OperationResult::fail("无法完整写入文件", file.errorString());
    // Recheck immediately before replacing the file; do not trust a pre-dialog snapshot.
    const auto stamp = sourcePreviewStamp(path);
    if (!sourcePreviewPath(request.location).result.success || !stamp.result.success || stamp.value != request.expectedStamp ||
        sourcePreviewIdentity(request.location.root) != request.location.rootIdentity)
        return OperationResult::fail("文件已被修改", "原文件未被覆盖，请重新读取后处理。");
    if (m_cancel && m_cancel->load()) return OperationResult::cancel("操作已取消");
    if (!file.commit()) return OperationResult::fail("保存失败", file.errorString());
    return OperationResult::ok("已保存到本地");
}
BackupResult<SourceMutationRequest> BackupEngine::prepareSourceMutation(const SourceLocation &where, SourceMutationKind kind, const QString &name)
{
    const auto record = sourceRecord(where);
    if (!record.result.success) return {record.result};
    if (!where.rootDirectory || (where.path.isEmpty() && kind != SourceMutationKind::MakeDirectory))
        return {OperationResult::fail("不能修改追踪根目录", "文件管理仅限追踪文件夹内部。")};
    const auto resolved = sourcePreviewPath(where);
    if (!resolved.result.success) return {resolved.result};
    if (kind != SourceMutationKind::Delete && !validSourceEntryName(name))
        return {OperationResult::fail("名称无效", "请输入单个文件或文件夹名称，不要包含路径或保留名称。")};
    SourceMutationRequest request{where, kind, name, {}};
    if (kind == SourceMutationKind::MakeDirectory)
    {
        if (!QFileInfo(resolved.value).isDir()) return {OperationResult::fail("不是目录", {})};
        request.snapshot.insert("parent", sourcePreviewIdentity(resolved.value));
    }
    else
    {
        const auto snapshot = sourceSnapshot(resolved.value);
        if (!snapshot.result.success) return {snapshot.result};
        request.snapshot = snapshot.value;
    }
    if (kind != SourceMutationKind::Delete)
    {
        const auto parent = kind == SourceMutationKind::MakeDirectory ? resolved.value : QFileInfo(resolved.value).absolutePath();
        if (BackupFiles::exists(QDir(parent).filePath(name))) return {OperationResult::fail("同名项目已存在", name)};
    }
    return {OperationResult::ok({}), request};
}
OperationResult BackupEngine::applySourceMutation(const SourceMutationRequest &request)
{
    const auto prepared = prepareSourceMutation(request.location, request.kind, request.name);
    if (!prepared.result.success) return prepared.result;
    if (request.snapshot.isEmpty() || request.snapshot != prepared.value.snapshot)
        return OperationResult::fail("目标已变化", "确认后内容发生变化，请重新检查再操作。");
    const auto resolved = sourcePreviewPath(request.location);
    if (!resolved.result.success) return resolved.result;
    const auto path = resolved.value;
    if (m_cancel && m_cancel->load()) return OperationResult::cancel("操作已取消");
    if (request.kind == SourceMutationKind::MakeDirectory)
        return QDir(path).mkdir(request.name) ? OperationResult::ok("文件夹已创建") : OperationResult::fail("创建失败", "请检查权限或同名项目。");
    if (request.kind == SourceMutationKind::Rename)
    {
        const auto target = QDir(QFileInfo(path).absolutePath()).filePath(request.name);
        const auto result = m_dependencies.files->rename(path, target);
        return result.success ? OperationResult::ok("已重命名") : result;
    }
    // Never change source permissions or recurse into links/metadata. Validate each
    // remaining directory's membership so files added during deletion are preserved.
    std::function<OperationResult(const QString &, const QString &)> remove = [&](const QString &current, const QString &relative) {
        if (m_cancel && m_cancel->load()) return OperationResult::cancel("删除已停止", "请检查已删除及剩余内容：" + path);
        if (BackupFiles::hasLinkedAncestor(current) || BackupFiles::isGitMetadataPath(current))
            return OperationResult::fail("删除已停止", "路径已变化，请检查剩余内容：" + current);
        const QFileInfo info(current);
        if (info.isDir())
        {
            const auto expected = request.snapshot.value(relative + '/');
            if (expected != sourcePreviewIdentity(current) + ':' + QByteArray::number(info.permissions()))
                return OperationResult::fail("删除已停止", "目录身份已变化：" + current);
            const auto children = m_dependencies.files->children(current);
            if (!children.result.success) return children.result;
            for (const auto &child : children.value)
            {
                const auto key = relative + '/' + QFileInfo(child).fileName();
                if (!request.snapshot.contains(key) && !request.snapshot.contains(key + '/'))
                    return OperationResult::fail("删除已停止", "出现新内容，已保留：" + child);
            }
            for (const auto &child : children.value)
            {
                const auto result = remove(child, relative + '/' + QFileInfo(child).fileName());
                if (!result.success) return result;
            }
            if (!QDir().rmdir(current)) return OperationResult::fail("无法删除目录", current);
        }
        else
        {
            const auto stamp = sourcePreviewStamp(current);
            if (!stamp.result.success || stamp.value != request.snapshot.value(relative))
                return OperationResult::fail("删除已停止", "文件已变化，已保留：" + current);
            if (!QFile::remove(current)) return OperationResult::fail("无法删除文件", current);
        }
        return OperationResult::ok("已永久删除");
    };
    auto result = remove(path, {});
    if (!result.success) result.message += "\n删除可能已部分完成，请刷新检查剩余内容。";
    return result;
}
