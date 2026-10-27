#include "backup_engine.h"
#include "backup_schemes.h"
#include <QScopeGuard>
#include <QSet>

namespace
{
enum class TransferFailure { None, Network, Authentication, Changed, Other };
TransferFailure transferFailure(const GitResult &result)
{
    const auto error = (result.error + '\n' + result.output).toLower();
    for (const auto *text : {"authentication failed", "permission denied (publickey", "could not read username", "could not read password", "access denied", "http 401", "error: 401", "error: 403"})
        if (error.contains(QLatin1String(text))) return TransferFailure::Authentication;
    for (const auto *text : {"non-fast-forward", "fetch first", "stale info", "cannot lock ref", "failed to update ref"})
        if (error.contains(QLatin1String(text))) return TransferFailure::Changed;
    for (const auto *text : {"could not resolve", "unable to access", "connection", "timed out", "timeout", "network", "remote end hung up", "unexpected disconnect", "执行超时"})
        if (error.contains(QLatin1String(text))) return TransferFailure::Network;
    return TransferFailure::Other;
}
}

OperationResult BackupEngine::setAutoSync(const QString &id, bool enabled)
{
    const auto checked = require(id);
    if (!checked.result.success) return checked.result;
    auto record = checked.value;
    if (enabled && record.sync.remoteUrl.isEmpty()) return OperationResult::warn("请先连接共享仓库", "填写云端地址并连接后，即可开启自动同步。");
    record.sync.automatic = enabled;
    if (!enabled) record.syncContinuation = false;
    if (enabled && record.state == BackupSyncState::Tracking)
    {
        record.sync.state = SyncState::Waiting;
        record.sync.detail.clear();
    }
    const auto saved = m_catalog.save(record);
    return saved.success ? OperationResult::ok(enabled ? "自动同步已开启" : "自动同步已关闭", enabled ? "保存后自动同步，每 30 秒检查协作者更新。" : "仍可手动同步，本地自动备份继续运行。") : saved;
}

BackupResult<SyncResult> BackupEngine::synchronize(const QString &id, const SyncRequestOptions &options,
                                                  const std::function<void(SyncPhase)> &progress, const std::function<bool()> &editing)
{
    auto checked = require(id);
    if (!checked.result.success) return {checked.result};
    auto status = checked.value.sync;
    QStringList schemeWarnings;
    status.branchRef.clear(); status.branches.clear();
    const auto finish = [&](OperationResult result, SyncState state, const QString &branch = QString()) -> BackupResult<SyncResult>
    {
        schemeWarnings.removeDuplicates();
        if (!schemeWarnings.isEmpty()) result.warning = (schemeWarnings + (result.warning.isEmpty() ? QStringList{} : QStringList{result.warning})).join('\n');
        auto record = *m_catalog.find(id);
        status.state = record.state == BackupSyncState::NeedsAttention ? SyncState::NeedsAttention : state;
        status.phase = SyncPhase::Idle;
        status.message = result.title;
        status.detail = result.message;
        if (!result.warning.isEmpty()) status.detail += (status.detail.isEmpty() ? QString() : QStringLiteral("\n")) + result.warning;
        status.branchRef = branch;
        status.automatic = record.sync.automatic;
        if (state == SyncState::Synced) { status.lastSuccess = QDateTime::currentDateTimeUtc(); record.syncContinuation = false; }
        record.sync = status;
        const auto saved = m_catalog.save(record);
        if (!saved.success)
        {
            status.state = SyncState::NeedsAttention; status.detail = saved.message;
            return {saved, {status}};
        }
        return {result, {status}};
    };
    if (options.trigger == SyncTrigger::Automatic && !status.automatic)
        return {OperationResult::cancel("自动同步已关闭"), {status}};
    if (checked.value.state == BackupSyncState::NeedsAttention)
        return finish(OperationResult::warn("需要先检查备份", checked.value.stateDetail), SyncState::NeedsAttention);
    if (checked.value.state != BackupSyncState::Tracking)
        return finish(OperationResult::info("需要处理差异", "请继续处理已保存的差异，完成后再同步全部方案。"), SyncState::NeedsResolution);

    const auto editingResult = [&] { return finish(OperationResult::info("等待保存编辑", "文件有未保存的编辑，保存或放弃后继续同步。"), SyncState::Editing); };
    if (editing && editing()) return editingResult();
    const auto originalRunner = m_dependencies.git;
    TransferFailure transfer = TransferFailure::None;
    m_dependencies.git = [&](const QString &path, const QStringList &args, const GitOptions &gitOptions)
    {
        const auto result = originalRunner(path, args, gitOptions);
        QString command;
        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "-c") { ++i; continue; }
            if (args[i].startsWith('-')) continue;
            command = args[i]; break;
        }
        if (!result.success() && !result.cancelled &&
            (command == "fetch" || command == "push" || command == "ls-remote")) transfer = transferFailure(result);
        return result;
    };
    const auto restoreRunner = qScopeGuard([&] { m_dependencies.git = originalRunner; });
    const auto failure = [&](const OperationResult &result) -> BackupResult<SyncResult>
    {
        auto state = result.cancelled ? SyncState::Waiting : result.retryable || transfer == TransferFailure::Network || transfer == TransferFailure::Changed ? SyncState::RetryLater : SyncState::NeedsAttention;
        const auto record = *m_catalog.find(id);
        if (!record.tagRemotes.value(record.tagEndpoint).conflicts.isEmpty()) state = SyncState::NeedsTags;
        if (record.state == BackupSyncState::ResolutionPending) state = SyncState::NeedsResolution;
        auto notice = result;
        if (!result.cancelled && transfer != TransferFailure::None)
        {
            notice.warning = result.message + (result.warning.isEmpty() ? QString() : '\n' + result.warning);
            switch (transfer)
            {
            case TransferFailure::Network: notice.message = "暂时无法连接共享仓库。本地版本已保留，稍后可以继续同步。"; break;
            case TransferFailure::Authentication: notice.message = "无法访问共享仓库，请检查登录凭据和仓库访问权限后重试。"; break;
            case TransferFailure::Changed: notice.message = "协作者仍在更新。本地版本已保留，稍后重新整理双方的更新。"; break;
            default: notice.message = "共享仓库未能完成本次操作，请检查连接或访问权限。"; break;
            }
        }
        return finish(notice, state);
    };
    auto git = repository(id);
    const auto remote = git.remoteName();
    if (!remote.result.success) return failure(remote.result);
    const auto address = git.run({"remote", "get-url", remote.value});
    if (!address.success()) return finish(OperationResult::warn("尚未连接共享仓库", "请先填写云端仓库地址并连接。"), SyncState::Disconnected);
    status.remoteUrl = address.output.trimmed();
    const auto endpoint = git.remoteEndpoint(remote.value);
    if (!endpoint.result.success) return failure(endpoint.result);
    const auto phase = [&](SyncPhase value) { if (progress) progress(value); };
    phase(SyncPhase::Saving);
    const auto savedSource = prepareBackup(id, true);
    if (!savedSource.result.success) return failure(savedSource.result);
    const auto committed = finishBackup(savedSource.value, "同步前保存本地修改");
    if (!committed.success) return failure(committed);
    // finishBackup clears file cancellation for its cleanup; subsequent captures must be cancellable again.
    m_dependencies.files->cancellation = m_cancel;
    auto record = *m_catalog.find(id);
    record.syncContinuation = true;
    record.sync.remoteUrl = status.remoteUrl;
    auto saved = m_catalog.save(record);
    if (!saved.success) return failure(saved);

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        transfer = TransferFailure::None;
        record = *m_catalog.find(id);
        phase(SyncPhase::Fetching);
        auto result = fetchBranches({id, record.branchRef, record.lastCommit, record.generation, record.branchVersion});
        if (!result.success) return failure(result);
        result = syncTags(id, false);
        if (!result.success) return failure(result);
        record = *m_catalog.find(id);
        phase(SyncPhase::Analyzing);
        const auto directory = BackupSchemes::read(record, git);
        if (!directory.result.success) return failure(directory.result);
        auto plan = directory.value;
        record.operation = "reconcile-schemes";
        saved = m_catalog.save(record); if (!saved.success) return failure(saved);
        result = BackupSchemes::reconcile(record, git, plan);
        if (!result.success) return failure(attention(record, result.message));
        record.operation.clear();
        saved = m_catalog.save(record); if (!saved.success) return failure(saved);
        schemeWarnings += plan.warnings;
        const auto snapshot = git.branches();
        if (!snapshot.result.success) return failure(snapshot.result);
        QString currentRemote;
        QStringList updates;
        status.branches.clear();
        QMap<QString, int> rows;
        for (const auto &branch : snapshot.value)
            if (!branch.remoteBranch)
            {
                rows[branch.ref] = status.branches.size();
                status.branches.append(branch.name + " · 等待检查");
            }
        const auto worktrees = git.run({"worktree", "list", "--porcelain"});
        if (!worktrees.success()) return failure(GitRepository::outcome(worktrees));
        for (const auto &branch : snapshot.value)
        {
            if (branch.remoteBranch) continue;
            auto &detail = status.branches[rows.value(branch.ref)];
            const auto theirs = plan.effectiveHeads.value(branch.name);
            if (branch.current) currentRemote = theirs;
            if (theirs.isEmpty()) { detail = branch.name + " · 待首次共享"; continue; }
            if (theirs == branch.head) { detail = branch.name + " · 版本一致"; continue; }
            const auto remoteBehind = git.run({"merge-base", "--is-ancestor", theirs, branch.head});
            if (remoteBehind.success()) { detail = branch.name + " · 有本地更新"; continue; }
            if (!remoteBehind.finished || remoteBehind.exitCode != 1) return failure(GitRepository::outcome(remoteBehind));
            if (branch.current) { detail = branch.name + " · 正在整理双方更新"; continue; }
            if (worktrees.output.split('\n').contains("branch " + branch.ref))
                return finish(OperationResult::warn("方案正在其他工作目录使用", "请先关闭或切换使用“" + branch.name + "”的其他 Git 工作目录。"), SyncState::NeedsAttention, branch.ref);
            const auto localBehind = git.run({"merge-base", "--is-ancestor", branch.head, theirs});
            QString merged = theirs;
            if (!localBehind.success())
            {
                if (!localBehind.finished || localBehind.exitCode != 1) return failure(GitRepository::outcome(localBehind));
                const auto combined = BackupSchemes::merge(git, m_catalog.repoPath(id), branch.head, theirs, m_dependencies.git, m_cancel);
                if (!combined.result.success) return failure(combined.result);
                if (combined.value.isEmpty())
                {
                    detail = branch.name + " · 内容有冲突";
                    return finish(OperationResult::info("需要处理方案内容", "方案“" + branch.name + "”有内容冲突，请切换到该方案选择要保留的内容。"), SyncState::NeedsBranch, branch.ref);
                }
                merged = combined.value;
            }
            updates.append("update " + branch.ref + ' ' + merged + ' ' + branch.head);
            detail = branch.name + " · 等待接收云端更新";
        }
        if (!updates.isEmpty())
        {
            record.operation = "sync-branches";
            saved = m_catalog.save(record); if (!saved.success) return failure(saved);
            const auto updated = git.run({"update-ref", "--stdin"}, ("start\n" + updates.join('\n') + "\nprepare\ncommit\n").toUtf8());
            if (!updated.success())
            {
                record.operation.clear(); saved = m_catalog.save(record);
                return failure(saved.success ? GitRepository::outcome(updated) : saved);
            }
            record.operation.clear(); ++record.branchVersion;
            saved = m_catalog.save(record); if (!saved.success) return failure(saved);
            for (auto &detail : status.branches)
                if (detail.endsWith("等待接收云端更新")) detail.replace("等待接收云端更新", "已接收云端更新");
        }
        record = *m_catalog.find(id);
        if (!currentRemote.isEmpty() && currentRemote != record.lastCommit &&
            !git.run({"merge-base", "--is-ancestor", currentRemote, record.lastCommit}).success())
        {
            if (editing && editing()) return editingResult();
            SourceFingerprint analyzedSource;
            result = m_dependencies.files->fingerprint(record.sourcePath, record.directory, analyzedSource, false);
            if (!result.success) return failure(result);
            const auto changedInput = [&](const OperationResult &error) -> BackupResult<SyncResult>
            {
                const auto latest = *m_catalog.find(id);
                SourceFingerprint now;
                const auto read = m_dependencies.files->fingerprint(latest.sourcePath, latest.directory, now, false);
                if (read.success && now != analyzedSource && latest.state != BackupSyncState::NeedsAttention)
                {
                    if (latest.state == BackupSyncState::ResolutionPending)
                    {
                        const auto cancelled = cancelResolution(id, latest.resolutionSession);
                        if (!cancelled.success) return failure(cancelled);
                    }
                    return finish(OperationResult::info("文件仍在变化", "整理期间源文件有新的修改，已保留内容，稍后重新同步。"), SyncState::RetryLater);
                }
                return failure(error);
            };
            const auto prepared = prepareResolution(id, false, {}, currentRemote, true);
            if (!prepared.result.success) return changedInput(prepared.result);
            if (!prepared.value.id.isEmpty())
            {
                if (prepared.value.remaining())
                    return finish(OperationResult::info("需要处理差异", "双方修改了同一处内容。请选择要保留的内容，完成后自动继续全量同步。"), SyncState::NeedsResolution);
                const auto apply = prepareSyncApply(id, prepared.value.id, prepared.value.revision);
                if (!apply.result.success) return changedInput(apply.result);
                if (editing && editing())
                {
                    const auto cancelled = cancelResolution(id, prepared.value.id);
                    if (!cancelled.success) return failure(cancelled);
                    return editingResult();
                }
                phase(SyncPhase::Applying);
                result = applySync(apply.value);
                if (!result.success) return changedInput(result);
            }
        }
        if (git.remoteEndpoint(remote.value).value != endpoint.value || git.remoteName().value != remote.value)
            return finish(OperationResult::warn("共享仓库配置已变化", "请检查地址后重新同步。"), SyncState::NeedsAttention);
        phase(SyncPhase::Uploading);
        record = *m_catalog.find(id);
        result = BackupSchemes::publish(record, git, plan);
        if (!result.success)
        {
            if (transfer == TransferFailure::Changed && attempt == 0) continue;
            return failure(result);
        }
        saved = m_catalog.save(record); if (!saved.success) return failure(saved);
        result = syncTags(id, true);
        if (!result.success)
        {
            result.title = "版本已同步，里程碑尚未完成";
            return failure(result);
        }
        for (auto &branch : status.branches)
            if (!branch.endsWith("云端方案已获取")) branch = branch.section(" · ", 0, 0) + " · 已同步";
        auto complete = OperationResult::ok("已同步", "全部方案的已保存历史与里程碑已同步。", 2000);
        complete.warning = result.warning;
        return finish(complete, SyncState::Synced);
    }
    return finish(OperationResult::warn("协作者仍在更新", "本地版本已保留，将稍后继续同步。"), SyncState::RetryLater);
}
