#include "engine.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QStandardPaths>
#include <QUuid>

namespace Backup
{
namespace
{
QStringList gitMetadata(const QString &source, Cancellation cancel)
{
    QStringList paths;
    if (!QFileInfo(source).isDir())
        return paths;
    QStringList directories{source};
    while (!directories.isEmpty())
    {
        checkCancelled(cancel);
        const auto directory = directories.takeLast();
        for (const auto &entry : QDir(directory).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
        {
            if (entry.fileName().compare(".git", Qt::CaseInsensitive) == 0)
                paths.append(QDir(source).relativeFilePath(entry.absoluteFilePath()));
            else if (entry.isDir() && !entry.isSymLink())
                directories.append(entry.absoluteFilePath());
        }
    }
    paths.sort();
    return paths;
}
void moveMetadata(const QStringList &paths, const QString &from, const QString &to)
{
    for (const auto &path : paths)
    {
        const auto src = QDir(from).filePath(path), dst = QDir(to).filePath(path);
        const auto parent = path.left(path.lastIndexOf('/') + 1);
        const auto name = path.mid(path.lastIndexOf('/') + 1);
        if (!parent.isEmpty())
            validateRelative(parent.chopped(1));
        if (name.compare(".git", Qt::CaseInsensitive) != 0)
            throw Error(ErrorCode::RecoveryRequired, "排除路径事务记录无效");
        if (QFileInfo::exists(src) || QFileInfo(src).isSymLink())
        {
            if (!QFileInfo(QFileInfo(dst).absolutePath()).isDir())
                throw Error(ErrorCode::RecoveryRequired, "Git 元数据的目标目录不可用：" + dst);
            renamePath(src, dst);
        }
        else if (!QFileInfo::exists(dst) && !QFileInfo(dst).isSymLink())
            throw Error(ErrorCode::RecoveryRequired, "Git 元数据丢失，已保留恢复材料：" + path);
    }
}
} // namespace
Engine::Engine(QString root, Tracking tracking) : m_root(std::move(root)), m_tracking(std::move(tracking)) {}
QString Engine::objectPath() const { return QDir(m_root).filePath("objects/" + m_tracking.id); }
QString Engine::repositoryPath() const { return QDir(objectPath()).filePath("repository.git"); }
QString Engine::journalPath() const { return QDir(objectPath()).filePath("transaction.json"); }
SourceIndex Engine::scanner() const
{
    // The Documents application directory is excluded as opaque application data; it is never opened or migrated.
    return SourceIndex({m_root, QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("ZcVersionBox")});
}
void Engine::checkpoint(const QString &name) const
{
    if (m_fault)
        m_fault(name);
}
void Engine::saveJournal(const QJsonObject &journal) const
{
    auto j = journal;
    j["version"] = 1;
    writeJson(journalPath(), j);
}
void Engine::finishPublication(TaskResult &result, const QString &revision, const QString &checkpointName)
{
    result.revision = revision;
    result.changed = true;
    try
    {
        checkpoint(checkpointName);
        recover();
    }
    catch (const Error &e)
    {
        // Publication is irreversible here. Only cleanup may be retried, never the original restore/rebuild.
        result.code = ErrorCode::RecoveryRequired;
        result.data["recoveryRequired"] = true;
        result.data["recoveryRetryable"] = e.retryable;
        result.message = "版本已发布，事务清理尚未完成：" + e.message;
    }
}
void Engine::recover()
{
    if (!QFileInfo::exists(journalPath()))
        return;
    m_index.reset();
    auto j = readJson(journalPath());
    if (j["version"].toInt() != 1)
        throw Error(ErrorCode::RecoveryRequired, "未知事务格式，已保留恢复材料");
    const auto type = j["type"].toString(), base = j["base"].toString(), next = j["next"].toString();
    if (type == "rebuild")
    {
        const auto staged = QDir(objectPath()).filePath("repository.next.git"), old = QDir(objectPath()).filePath("repository.previous.git");
        if (!QFileInfo::exists(repositoryPath()) && QFileInfo::exists(old))
            renamePath(old, repositoryPath());
        const auto current = SnapshotStore(repositoryPath()).head();
        if (current != base && current != next)
            throw Error(ErrorCode::RecoveryRequired, "重建事务与仓库状态不符");
        if (j["cleanupStarted"].toBool() && current != next)
            throw Error(ErrorCode::RecoveryRequired, "重建清理阶段的仓库引用已改变");
        removeOwnedPath(staged);
        if (current == next)
        {
            if (!j["cleanupStarted"].toBool())
            {
                if (!QFileInfo::exists(old) || SnapshotStore(old).head() != base)
                    throw Error(ErrorCode::RecoveryRequired, "重建前的仓库已改变或丢失");
                j["cleanupStarted"] = true;
                saveJournal(j);
            }
            checkpoint("rebuildCleanup");
            removeOwnedPath(old);
        }
        else if (QFileInfo::exists(old))
            throw Error(ErrorCode::RecoveryRequired, "存在未确认的原仓库副本");
    }
    else
    {
        SnapshotStore store(repositoryPath());
        const auto current = store.head();
        if (current != base && (next.isEmpty() || current != next))
            throw Error(ErrorCode::RecoveryRequired, "事务期间仓库已被外部修改，已保留恢复材料");
        if (type == "replace")
            recoverReplacement(j, current);
        else if (type != "snapshot")
            throw Error(ErrorCode::RecoveryRequired, "未知事务类型");
        const auto pending = j["pending"].toString();
        if (!pending.isEmpty())
        {
            if (!pending.startsWith("refs/zcversionbox/pending/"))
                throw Error(ErrorCode::RecoveryRequired, "事务引用无效");
            git(repositoryPath(), {"update-ref", "-d", pending});
        }
    }
    removeOwnedPath(journalPath());
}
void Engine::recoverReplacement(QJsonObject journal, const QString &current)
{
    const auto source = m_tracking.source;
    const auto token = journal["token"].toString();
    if (QUuid(token).isNull() || !journal["gitMetadata"].isArray())
        throw Error(ErrorCode::RecoveryRequired, "事务路径记录无效");
    auto phase = journal["phase"].toString();
    if (!QStringList{"preparing", "movingOriginal", "installing", "installed", "rollbackRemoving", "rollbackRestoring", "rollbackRestored", "publishedCleanup"}.contains(phase))
        throw Error(ErrorCode::RecoveryRequired, "事务替换阶段无效");
    const auto parent = QFileInfo(source).absolutePath();
    const auto prefix = "." + QFileInfo(source).fileName() + ".zcversionbox-" + token;
    const auto staged = QDir(parent).filePath(prefix + "-staging"), rollback = QDir(parent).filePath(prefix + "-rollback");
    QStringList metadata;
    for (const auto &path : journal["gitMetadata"].toArray())
        metadata.append(path.toString());
    auto exists = [](const QString &path)
    {
        return QFileInfo::exists(path) || QFileInfo(path).isSymLink();
    };
    auto setPhase = [&](const QString &nextPhase)
    {
        journal["phase"] = nextPhase;
        saveJournal(journal);
        phase = nextPhase;
    };
    const bool hadSource = journal.contains("before");
    const auto before = hadSource ? Snapshot::parse(journal["before"].toObject()) : Snapshot();
    const auto validateOriginal = [&]
    {
        if (!hadSource || !exists(rollback) || !scanner().scan(rollback).sameContent(before))
            throw Error(ErrorCode::RecoveryRequired, "原内容副本已改变或丢失：" + rollback);
    };
    if (current == journal["next"].toString())
    {
        if (phase != "publishedCleanup")
        {
            if (phase != "installed" || !exists(source) || exists(staged))
                throw Error(ErrorCode::RecoveryRequired, "已发布版本与源替换阶段不符");
            if (hadSource)
            {
                validateOriginal();
                for (const auto &path : gitMetadata(rollback, {}))
                    if (!metadata.contains(path))
                        throw Error(ErrorCode::RecoveryRequired, "原目录出现新的 Git 元数据，已保留恢复材料：" + path);
                moveMetadata(metadata, rollback, source);
            }
            else if (exists(rollback))
                throw Error(ErrorCode::RecoveryRequired, "导入事务出现未知原内容副本");
            setPhase("publishedCleanup");
        }
        // After this durable intent, partial deletion is expected and can safely continue.
        checkpoint("replacementCleanup");
        removeOwnedPath(rollback);
        removeOwnedPath(staged);
        return;
    }
    if (phase == "publishedCleanup")
        throw Error(ErrorCode::RecoveryRequired, "清理阶段的已发布引用已改变");
    if (phase == "preparing" || (phase == "movingOriginal" && !exists(rollback)))
    {
        // Nothing was installed. Even an identical concurrently created destination is not ours.
        if (exists(rollback) || (hadSource && !exists(source)))
            throw Error(ErrorCode::RecoveryRequired, "替换前的源内容状态不符");
        removeOwnedPath(staged);
        return;
    }
    if (phase == "rollbackRestored")
    {
        if (exists(rollback))
            throw Error(ErrorCode::RecoveryRequired, "回滚完成后仍存在原内容副本");
        removeOwnedPath(staged);
        return;
    }
    if (phase == "rollbackRestoring" && !exists(rollback))
    {
        if (!exists(source) || !scanner().scan(source).sameContent(before))
            throw Error(ErrorCode::RecoveryRequired, "回滚后的源内容发生变化");
        setPhase("rollbackRestored");
        removeOwnedPath(staged);
        return;
    }
    if (hadSource)
        validateOriginal();
    else if (exists(rollback))
        throw Error(ErrorCode::RecoveryRequired, "导入事务出现未知原内容副本");
    const bool installed = phase == "installed" || phase == "rollbackRemoving" || (phase == "installing" && !exists(staged));
    if (installed)
    {
        if (phase != "rollbackRemoving")
        {
            if (exists(source))
            {
                if (!scanner().scan(source).sameContent(SnapshotStore(repositoryPath()).readSnapshot(journal["next"].toString())))
                    throw Error(ErrorCode::RecoveryRequired, "源内容已改变，无法自动回滚：" + source);
                if (hadSource)
                    moveMetadata(metadata, source, rollback);
            }
            setPhase("rollbackRemoving");
        }
        checkpoint("rollbackCleanup");
        removeOwnedPath(source);
    }
    else if (exists(source))
        throw Error(ErrorCode::RecoveryRequired, "源位置出现并发内容，已保留恢复材料");
    if (hadSource)
    {
        setPhase("rollbackRestoring");
        renamePath(rollback, source);
    }
    setPhase("rollbackRestored");
    removeOwnedPath(staged);
}
QString Engine::snapshot(bool full, const QStringList &dirty, const QString &operation, TaskResult &result)
{
    SnapshotStore store(repositoryPath(), m_cancel);
    const auto base = store.head();
    const auto nextIndex = scanner().scan(m_tracking.source, (!full && m_index) ? &*m_index : nullptr, dirty, m_cancel, &result);
    if (!base.isEmpty() && nextIndex.sameContent(store.readSnapshot(base)))
    {
        m_index = nextIndex;
        return base;
    }
    const auto pending = "refs/zcversionbox/pending/" + newId();
    QJsonObject j{{"type", "snapshot"}, {"base", base}, {"pending", pending}};
    saveJournal(j);
    checkpoint("beforeObjects");
    const auto revision = store.candidate(nextIndex, m_tracking.source, base, pending, operation, &result);
    j["next"] = revision;
    saveJournal(j);
    checkpoint("afterObjects");
    checkCancelled(m_cancel);
    store.publish(revision, base);
    finishPublication(result, revision, "afterPublish");
    m_index = nextIndex;
    return revision;
}
void Engine::replaceSource(const QString &next, const QString &base, const std::optional<Snapshot> &before, TaskResult &result)
{
    // A preceding protection snapshot must finish its journal before a replacement can start.
    recover();
    result.data.remove("recoveryRequired");
    result.data.remove("recoveryRetryable");
    result.code = ErrorCode::None;
    result.message.clear();
    SnapshotStore store(repositoryPath(), m_cancel);
    const auto after = store.readSnapshot(next);
    const auto oldData = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("ZcVersionBox");
    if (containsPath(m_tracking.source, m_root) || containsPath(m_tracking.source, oldData))
        throw Error(ErrorCode::Unsupported, "源目录包含应用数据，请将所需版本导出到独立目录");
    const auto metadata = before ? gitMetadata(m_tracking.source, m_cancel) : QStringList();
    for (const auto &path : metadata)
    {
        const auto parent = path.left(path.lastIndexOf('/'));
        if (after.kind != "directory" || (path.contains('/') && !after.directories.contains(parent)))
            throw Error(ErrorCode::Conflict, "目标版本无法保留现有 Git 元数据：" + path);
    }
    const auto token = newId();
    const QFileInfo source(m_tracking.source);
    const auto prefix = "." + source.fileName() + ".zcversionbox-" + token;
    const auto staged = QDir(source.absolutePath()).filePath(prefix + "-staging"), rollback = QDir(source.absolutePath()).filePath(prefix + "-rollback");
    QJsonObject j{{"type", "replace"}, {"phase", "preparing"}, {"token", token}, {"base", base}, {"next", next}, {"gitMetadata", QJsonArray::fromStringList(metadata)}};
    if (before)
        j["before"] = before->json();
    saveJournal(j);
    store.exportSnapshot(next, staged);
    checkpoint("afterExport");
    if (before)
    {
        if (!scanner().scan(m_tracking.source, nullptr, {}, m_cancel).sameContent(*before) || gitMetadata(m_tracking.source, m_cancel) != metadata)
            throw Error(ErrorCode::Conflict, "替换前源文件发生变化");
        j["phase"] = "movingOriginal";
        saveJournal(j);
        renamePath(m_tracking.source, rollback);
    }
    else if (QFileInfo::exists(m_tracking.source) || QFileInfo(m_tracking.source).isSymLink())
        throw Error(ErrorCode::Conflict, "目标路径已出现内容，导入已停止");
    checkpoint("afterOriginalMoved");
    j["phase"] = "installing";
    saveJournal(j);
    renamePath(staged, m_tracking.source);
    j["phase"] = "installed";
    saveJournal(j);
    checkpoint("afterSourceSwap");
    moveMetadata(metadata, rollback, m_tracking.source);
    checkpoint("afterMetadataMoved");
    checkCancelled(m_cancel);
    if ((before && (!scanner().scan(rollback, nullptr, {}, m_cancel).sameContent(*before) || !gitMetadata(rollback, m_cancel).isEmpty())) ||
        !scanner().scan(m_tracking.source, nullptr, {}, m_cancel).sameContent(after))
        throw Error(ErrorCode::Conflict, "替换期间源文件发生并发修改，已保留双方内容");
    store.publish(next, base);
    finishPublication(result, next, "afterPublish");
    m_index = after;
}
void Engine::rebuild(TaskResult &result)
{
    SnapshotStore original(repositoryPath(), m_cancel);
    const auto base = original.head(), nextPath = QDir(objectPath()).filePath("repository.next.git"), oldPath = QDir(objectPath()).filePath("repository.previous.git");
    if (QFileInfo::exists(nextPath) || QFileInfo::exists(oldPath))
        throw Error(ErrorCode::RecoveryRequired, "存在未完成的重建材料");
    QJsonObject j{{"type", "rebuild"}, {"base", base}};
    saveJournal(j);
    const auto index = scanner().scan(m_tracking.source, nullptr, {}, m_cancel, &result);
    SnapshotStore next(nextPath, m_cancel);
    next.initialize();
    const auto revision = next.candidate(index, m_tracking.source, {}, "refs/zcversionbox/pending/rebuild", "重建 " + newId(), &result);
    next.publish(revision, {});
    git(nextPath, {"update-ref", "-d", "refs/zcversionbox/pending/rebuild"}, {}, m_cancel);
    j["next"] = revision;
    saveJournal(j);
    checkpoint("rebuildPrepared");
    renamePath(repositoryPath(), oldPath);
    checkpoint("rebuildOriginalMoved");
    renamePath(nextPath, repositoryPath());
    finishPublication(result, revision, "rebuildPublished");
    m_index = index;
}
TaskResult Engine::execute(const Request &r, Cancellation cancel)
{
    m_cancel = std::move(cancel);
    TaskResult result;
    result.requestId = r.requestId;
    result.trackingId = m_tracking.id;
    result.operation = r.operation;
    try
    {
        recover();
        checkCancelled(m_cancel);
        SnapshotStore store(repositoryPath(), m_cancel);
        if (!QFileInfo::exists(QDir(repositoryPath()).filePath("HEAD")))
            store.initialize();
        if (m_tracking.pendingImport && QStringList{"snapshot", "restore", "pull", "push", "resetRemote", "rebuild"}.contains(r.operation))
            throw Error(ErrorCode::Conflict, "请先完成或取消云端导入");
        if (r.operation == "snapshot")
            result.revision = snapshot(r.fullScan || !m_index, r.dirtyPaths, "自动备份", result);
        else if (r.operation == "history")
            result.data = store.history(r.offset, r.limit);
        else if (r.operation == "diff")
            result.data = store.diff(r.revision);
        else if (r.operation == "export")
        {
            const auto path = QDir(m_root).filePath("previews/" + newId() + '/' + store.readSnapshot(r.revision).name);
            store.exportSnapshot(r.revision, path, true);
            result.data["path"] = path;
        }
        else if (r.operation == "restore")
        {
            const bool exists = QFileInfo::exists(m_tracking.source) || QFileInfo(m_tracking.source).isSymLink();
            const auto base = exists ? snapshot(true, {}, "恢复前保护", result) : store.head();
            const std::optional<Snapshot> before = exists ? std::optional<Snapshot>(store.readSnapshot(base)) : std::nullopt;
            const auto next = store.copyCommit(r.revision, base, "恢复版本");
            replaceSource(next, base, before, result);
        }
        else if (r.operation == "import")
        {
            if (!store.head().isEmpty() && m_tracking.pendingImport)
            {
                result.revision = store.head();
                if (!scanner().scan(m_tracking.source).sameContent(store.readSnapshot(result.revision)))
                    throw Error(ErrorCode::Conflict, "已发布的导入内容发生变化");
                m_tracking.pendingImport = false;
                writeJson(QDir(objectPath()).filePath("config.json"), m_tracking.json());
                return result;
            }
            if (!store.head().isEmpty())
                throw Error(ErrorCode::Conflict, "该对象已经有本地历史");
            if (QFileInfo::exists(m_tracking.source) || QFileInfo(m_tracking.source).isSymLink())
                throw Error(ErrorCode::Conflict, "导入目标必须是尚不存在的路径");
            const auto remote = store.fetch(m_tracking.remote);
            store.syncNotes();
            replaceSource(remote, {}, std::nullopt, result);
            m_tracking.pendingImport = false;
            writeJson(QDir(objectPath()).filePath("config.json"), m_tracking.json());
        }
        else if (r.operation == "pull")
        {
            const auto local = snapshot(true, {}, "拉取前保护", result);
            const auto remote = store.fetch(m_tracking.remote);
            result.conflict = {"pull", "HistoryDiverged", {}, local, remote, {}};
            result.conflict.base = QString::fromLatin1(git(repositoryPath(), {"merge-base", local, remote}, {}, m_cancel, {0, 1})).trimmed();
            const auto changedPaths = git(repositoryPath(), {"diff", "--name-only", "-z", local, remote, "--", "payload"}, {}, m_cancel);
            for (const auto &path : changedPaths.split('\0'))
                if (!path.isEmpty())
                    result.conflict.paths.append(QString::fromUtf8(path).mid(8));
            const auto localNotes = store.head("refs/notes/zcversionbox"), remoteNotes = store.head("refs/remotes/cloud-notes/zcversionbox");
            if (!localNotes.isEmpty() && !remoteNotes.isEmpty() && !store.ancestor(localNotes, remoteNotes) && !store.ancestor(remoteNotes, localNotes))
            {
                result.conflict.category = "NotesDiverged";
                result.conflict.local = localNotes;
                result.conflict.remote = remoteNotes;
                result.conflict.base = QString::fromLatin1(git(repositoryPath(), {"merge-base", localNotes, remoteNotes}, {}, m_cancel, {0, 1})).trimmed();
                throw Error(ErrorCode::Conflict, "备注历史已分叉，等待冲突解决");
            }
            if (local != remote && !store.ancestor(local, remote) && !store.ancestor(remote, local))
                throw Error(ErrorCode::Conflict, "本地和远端历史已分叉，等待冲突解决");
            if (local != remote && store.ancestor(local, remote))
            {
                result.conflict.category = "SourceChanged";
                result.conflict.paths = {m_tracking.source};
                replaceSource(remote, local, store.readSnapshot(local), result);
            }
            else
                result.revision = local;
            store.syncNotes();
            result.conflict = {};
        }
        else if (r.operation == "push" || r.operation == "resetRemote")
        {
            if (r.operation == "resetRemote" && r.revision.isEmpty())
                throw Error(ErrorCode::InvalidFormat, "覆盖远端必须提供已确认的远端版本");
            result.revision = snapshot(true, {}, "推送前备份", result);
            store.push(m_tracking.remote, r.operation == "resetRemote" ? r.revision : QString());
        }
        else if (r.operation == "remoteHead")
        {
            result.revision = store.fetch(m_tracking.remote);
        }
        else if (r.operation == "setRemote")
        {
            if (r.remote.startsWith('-'))
                throw Error(ErrorCode::InvalidFormat, "远端地址无效");
            auto tracking = m_tracking;
            tracking.remote = r.remote;
            writeJson(QDir(objectPath()).filePath("config.json"), tracking.json());
            m_tracking = tracking;
            result.data["tracking"] = m_tracking.json();
        }
        else if (r.operation == "annotate" || r.operation == "annotateAi")
        {
            store.annotate(r.revision, r.text, r.operation == "annotateAi");
            result.revision = r.revision;
        }
        else if (r.operation == "rebuild")
            rebuild(result);
        else if (r.operation == "remove")
        {
            const auto trash = QDir(m_root).filePath("trash/" + m_tracking.id + '-' + newId());
            if (!QDir().mkpath(QFileInfo(trash).absolutePath()))
                throw Error(ErrorCode::Permission, "无法创建删除事务目录");
            renamePath(objectPath(), trash);
            result.data["removed"] = true;
            try
            {
                removeOwnedPath(trash);
            }
            catch (const Error &e)
            {
                result.message = "已移除追踪，待清理：" + e.message;
            }
        }
        else if (r.operation == "resolveConflict")
            throw Error(ErrorCode::Unsupported, "冲突解决系统尚未实现");
        else if (r.operation == "status" || r.operation == "recover" || r.operation == "initialize")
        {
            const auto revision = store.head();
            result.revision = revision;
            if (!revision.isEmpty())
            {
                const auto s = store.readSnapshot(revision);
                qint64 total = 0;
                for (const auto &f : s.files)
                    total += f.size;
                result.data = {{"files", s.files.size()}, {"bytes", QString::number(total)}, {"kind", s.kind}, {"name", s.name}};
            }
        }
        else
            throw Error(ErrorCode::Unsupported, "未知备份操作");
    }
    catch (const Error &e)
    {
        result.success = false;
        result.code = e.code;
        result.message = e.message;
        result.retryable = e.retryable;
        m_index.reset();
        if (e.code == ErrorCode::Conflict && result.conflict.category.isEmpty())
        {
            result.conflict = {r.operation, "SourceChanged", {}, {}, {}, {m_tracking.source}};
            if (r.operation == "push" || r.operation == "resetRemote")
            {
                result.conflict.category = "RemoteChanged";
                result.conflict.base = r.revision;
                try
                {
                    SnapshotStore store(repositoryPath());
                    result.conflict.local = store.head();
                    result.conflict.remote = store.fetch(m_tracking.remote);
                    const auto localNotes = store.head("refs/notes/zcversionbox"), remoteNotes = store.head("refs/remotes/cloud-notes/zcversionbox");
                    if (!localNotes.isEmpty() && !remoteNotes.isEmpty() && !store.ancestor(remoteNotes, localNotes))
                    {
                        result.conflict.category = "NotesDiverged";
                        result.conflict.local = localNotes;
                        result.conflict.remote = remoteNotes;
                        result.conflict.base = QString::fromLatin1(git(repositoryPath(), {"merge-base", localNotes, remoteNotes}, {}, {}, {0, 1})).trimmed();
                    }
                }
                catch (const Error &detail)
                {
                    result.message += "\n无法读取远端冲突详情：" + detail.message;
                }
            }
        }
        try
        {
            recover();
        }
        catch (const Error &recovery)
        {
            result.code = ErrorCode::RecoveryRequired;
            result.retryable = false;
            result.data["recoveryRequired"] = true;
            result.data["recoveryRetryable"] = recovery.retryable;
            result.message += "\n恢复需要处理：" + recovery.message;
        }
    }
    return result;
}
} // namespace Backup
