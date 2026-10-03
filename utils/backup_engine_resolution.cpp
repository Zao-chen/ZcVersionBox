#include "backup_engine.h"
#include "backup_merge.h"
#include <QScopeGuard>
#include <QUuid>

namespace
{
OperationResult invalidSession() { return OperationResult::warn("处理进度已变化", "请重新打开处理页面。"); }
QString progress(const SyncResolutionSession &session)
{
    return session.remaining() ? QString("还有 %1 / %2 处内容需要选择。自动备份已暂停。").arg(session.remaining()).arg(session.total())
                               : QStringLiteral("内容已整理，请预览并确认应用。自动备份已暂停。");
}
}
QString BackupEngine::resolutionPath(const BackupRecord &record) const
{
    return BackupCatalog::validId(record.resolutionSession) ? m_catalog.itemPath(record.id) + "/resolutions/" + record.resolutionSession : QString();
}
BackupResult<BackupRecord> BackupEngine::resolutionRecord(const QString &id, const QString &sessionId)
{
    const auto record = require(id);
    if (!record.result.success) return record;
    if (record.value.resolutionSession.isEmpty() || (!sessionId.isEmpty() && sessionId != record.value.resolutionSession)) return {invalidSession()};
    return record;
}
BackupResult<SyncResolutionSession> BackupEngine::prepareSyncResolution(const QString &id, bool restart)
{
    auto checked = require(id);
    if (!checked.result.success) return {checked.result};
    QString tagWarning;
    if (checked.value.state != BackupSyncState::NeedsAttention)
    {
        const auto tags = syncTags(id, false);
        if (!tags.success) return {tags};
        tagWarning = tags.warning;
        checked = require(id);
        if (!checked.result.success) return {checked.result};
    }
    auto before = checked.value;
    if (before.state == BackupSyncState::ResolutionPending && !restart) return syncResolution(id);
    if (before.state == BackupSyncState::NeedsAttention) return {OperationResult::warn("需要先检查备份", before.stateDetail)};
    if (before.state == BackupSyncState::ResolutionPending)
    {
        const auto clean = repository(id).clean(), head = repository(id).head().result;
        if (!clean.success) return {clean};
        if (!head.success || repository(id).head().value != before.resolutionHead) return {OperationResult::warn("需要检查", "仓库在应用外发生变化，不能重新分析。")};
    }
    else
    {
        checked = require(id, true, before.state == BackupSyncState::RemotePending);
        if (!checked.result.success) return {checked.result};
        before = checked.value;
    }
    auto record = before;
    record.resolutionSession = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.resolutionHead = repository(id).head().value;
    record.state = BackupSyncState::ResolutionPending;
    record.stateDetail = "正在整理本地与云端内容。源文件保持原样。";
    record.operation = "prepare-resolution";
    const auto root = resolutionPath(record);
    record.recoveryPaths = {root};
    auto saved = m_catalog.save(record);
    if (!saved.success) return {saved};
    BackupMerge merge(root, m_dependencies, m_cancel);
    const auto created = merge.create(before, m_catalog.repoPath(id), before.state == BackupSyncState::RemotePending ? before.pendingCommit : QString());
    if (!created.success)
    {
        const auto reset = m_catalog.save(before);
        if (reset.success) { m_dependencies.files->cancellation.reset(); m_dependencies.files->remove(root); }
        auto result = created;
        if (!reset.success) result.warning = "暂停状态未能恢复，原内容与处理材料已保留。";
        return {result};
    }
    const auto session = merge.session();
    if (session.total() == 0 && before.state == BackupSyncState::Tracking)
    {
        const auto ready = merge.prepare();
        if (ready.result.success && ready.value.commit == record.resolutionHead && ready.value.changes.isEmpty())
        {
            saved = m_catalog.save(before);
            if (!saved.success) return {saved};
            m_dependencies.files->remove(root);
            auto result = OperationResult::ok("已是最新", "历史版本与里程碑标记已更新，没有需要应用的文件变化。");
            result.warning = tagWarning;
            return {result, {}};
        }
    }
    record.pendingCommit = merge.remoteCommit(); record.operation.clear(); record.recoveryPaths.clear(); record.stateDetail = progress(session);
    saved = m_catalog.save(record);
    if (!saved.success) return {saved};
    if (!before.resolutionSession.isEmpty() && before.resolutionSession != record.resolutionSession)
        m_dependencies.files->remove(resolutionPath(before));
    auto result = OperationResult::info("同步差异已准备", record.stateDetail);
    result.warning = tagWarning;
    return {result, session};
}
BackupResult<SyncResolutionSession> BackupEngine::syncResolution(const QString &id)
{
    const auto initial = require(id);
    if (!initial.result.success) return {initial.result};
    if (initial.value.state == BackupSyncState::RemotePending) return prepareSyncResolution(id);
    const auto record = resolutionRecord(id);
    if (!record.result.success) return {record.result};
    BackupMerge merge(resolutionPath(record.value), m_dependencies, m_cancel);
    const auto loaded = merge.load();
    if (!loaded.success) return {loaded};
    auto result = merge.session();
    const auto verified = merge.verify(record.value, m_catalog.repoPath(id));
    result.stale = !verified.success || record.value.state != BackupSyncState::ResolutionPending;
    result.staleReason = !verified.success ? verified.message : result.stale ? record.value.stateDetail : QString();
    return {OperationResult::ok({}), result};
}
BackupResult<SyncResolutionSession> BackupEngine::chooseSyncResolution(const QString &id, const QString &sessionId, quint64 revision, const QString &path, int hunk, ConflictChoice choice)
{
    auto record = resolutionRecord(id, sessionId);
    if (!record.result.success) return {record.result};
    if (record.value.state != BackupSyncState::ResolutionPending) return {invalidSession()};
    BackupMerge merge(resolutionPath(record.value), m_dependencies, m_cancel);
    auto result = merge.load(); if (!result.success) return {result};
    result = merge.verify(record.value, m_catalog.repoPath(id)); if (!result.success) return {result};
    result = merge.choose(revision, path, hunk, choice); if (!result.success) return {result};
    record.value.stateDetail = progress(merge.session());
    result = m_catalog.save(record.value); if (!result.success) return {result};
    return {OperationResult::ok({}), merge.session()};
}
BackupResult<PreparedSyncApply> BackupEngine::prepareSyncApply(const QString &id, const QString &sessionId, quint64 revision)
{
    const auto record = resolutionRecord(id, sessionId);
    if (!record.result.success) return {record.result};
    if (record.value.state != BackupSyncState::ResolutionPending) return {invalidSession()};
    BackupMerge merge(resolutionPath(record.value), m_dependencies, m_cancel);
    auto result = merge.load(); if (!result.success) return {result};
    if (merge.session().revision != revision) return {invalidSession()};
    result = merge.verify(record.value, m_catalog.repoPath(id)); if (!result.success) return {result};
    return merge.prepare();
}
OperationResult BackupEngine::applySync(const PreparedSyncApply &request)
{
    const auto checked = resolutionRecord(request.id, request.sessionId);
    if (!checked.result.success) return checked.result;
    const auto before = checked.value;
    if (before.state != BackupSyncState::ResolutionPending) return invalidSession();
    BackupMerge merge(resolutionPath(before), m_dependencies, m_cancel);
    auto result = merge.load(); if (!result.success) return result;
    if (!merge.validRequest(request)) return invalidSession();
    result = merge.verify(before, m_catalog.repoPath(request.id)); if (!result.success) return result;
    result = validateSource(before.sourcePath, true); if (!result.success) return result;
    BackupReplacement replacement(m_dependencies.files, before.sourcePath);
    if (!replacement.valid()) return OperationResult::fail("应用失败", "无法准备源文件恢复副本。");
    result = merge.exportSource(request, replacement.stagingPath()); if (!result.success) return result;
    result = merge.verify(before, m_catalog.repoPath(request.id)); if (!result.success) return result;
    auto record = before;
    record.operation = "apply-resolution";
    record.recoveryPaths = {replacement.recoveryPath(), resolutionPath(before)};
    result = m_catalog.save(record); if (!result.success) return result;
    auto live = repository(request.id);
    const auto stopped = [&](const OperationResult &failure)
    {
        m_dependencies.files->cancellation.reset();
        replacement.preserve();
        return attention(record, failure.message + "。原内容和处理副本已保留，请检查后再继续。");
    };
    const auto rollbackBeforeCommit = [&](const OperationResult &failure)
    {
        const auto head = live.head();
        if (!head.result.success || head.value != merge.expectedHead() || !live.clean().success) return stopped(failure);
        const auto restored = m_catalog.save(before);
        if (!restored.success) return stopped(restored);
        return failure;
    };
    result = GitRepository::outcome(live.run({"fetch", "--no-tags", "--", merge.repositoryPath(), "refs/heads/resolution-result"}));
    if (!result.success) return rollbackBeforeCommit(result);
    const auto fetched = live.resolve("FETCH_HEAD");
    if (!fetched.result.success || fetched.value != request.commit) return rollbackBeforeCommit(invalidSession());
    result = merge.verify(before, m_catalog.repoPath(request.id)); if (!result.success) return rollbackBeforeCommit(result);
    result = GitRepository::outcome(live.run({"merge", "--ff-only", "--no-edit", request.commit}));
    if (!result.success) return rollbackBeforeCommit(result);
    if (live.head().value != request.commit || !live.clean().success) return stopped(invalidSession());
    // The user may edit the source while Git finishes. Recheck before the first source move.
    SourceFingerprint current;
    result = m_dependencies.files->fingerprint(before.sourcePath, before.directory, current, false);
    if (!result.success || current != request.sourceFingerprint) return stopped(OperationResult::fail("应用已暂停", "源内容在确认期间发生变化"));
    result = replacement.install();
    if (!result.success)
    {
        m_dependencies.files->cancellation.reset();
        const auto rolled = replacement.rollback();
        if (!rolled.success) result.warning = rolled.message;
        return stopped(result);
    }
    m_dependencies.files->cancellation.reset();
    result = replacement.verifyInstalled(); if (!result.success) return stopped(result);
    SourceFingerprint fingerprint;
    result = m_dependencies.files->fingerprint(before.sourcePath, before.directory, fingerprint); if (!result.success) return stopped(result);
    result = replacement.verifyInstalled(); if (!result.success) return stopped(result);
    if (live.head().value != request.commit || !live.clean().success) return stopped(OperationResult::fail("应用已暂停", "仓库在应用期间发生变化"));
    record.lastCommit = request.commit; record.fingerprint = fingerprint; record.state = BackupSyncState::Tracking;
    record.pendingCommit.clear(); record.resolutionSession.clear(); record.resolutionHead.clear(); record.stateDetail.clear(); record.operation.clear(); record.recoveryPaths.clear();
    result = m_catalog.save(record);
    if (!result.success) { replacement.preserve(); return result; }
    auto completed = OperationResult::ok("同步差异已处理", "已应用并保存版本，自动备份已恢复。云端上传可另行操作。");
    const auto cleaned = replacement.finish(); if (!cleaned.success) completed.warning = cleaned.message;
    const auto removed = m_dependencies.files->remove(resolutionPath(before)); if (!removed.success) completed.warning += removed.message;
    return completed;
}
BackupResult<ConflictContent> BackupEngine::syncContent(const QString &id, const QString &sessionId, const QString &path, ConflictSide side)
{
    const auto record = resolutionRecord(id, sessionId); if (!record.result.success) return {record.result};
    BackupMerge merge(resolutionPath(record.value), m_dependencies, m_cancel);
    const auto loaded = merge.load(); if (!loaded.success) return {loaded};
    return merge.content(path, side);
}
OperationResult BackupEngine::previewSync(const QString &id, const QString &sessionId, const QString &path, ConflictSide side)
{
    const auto record = resolutionRecord(id, sessionId); if (!record.result.success) return record.result;
    BackupMerge merge(resolutionPath(record.value), m_dependencies, m_cancel);
    const auto loaded = merge.load(); if (!loaded.success) return loaded;
    QTemporaryDir temp(QDir(QDir::tempPath()).canonicalPath() + "/ZcVersionBoxChoice-XXXXXX");
    if (!temp.isValid()) return OperationResult::fail("预览失败", "无法创建只读副本。");
    const auto target = temp.path() + '/' + QFileInfo(path).fileName();
    auto result = merge.exportSide(path, side, target); if (!result.success) return result;
    result = m_dependencies.files->readOnly(temp.path()); if (!result.success) return result;
    result.path = target;
    m_previews.append(temp.path()); temp.setAutoRemove(false);
    return result;
}
