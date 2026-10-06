#include "backup_engine.h"
#include <QDir>
#include <QFileInfo>

namespace
{
OperationResult staleBranch() { return OperationResult::warn("请重新确认方案操作", "当前方案、版本或目标分支已经变化，请刷新后重试。"); }
}

BackupResult<BackupRecord> BackupEngine::requireBranch(const BranchContext &context)
{
    const auto record = require(context.id, true);
    if (!record.result.success) return record;
    const auto &r = record.value;
    if (context.generation != r.generation || context.version != r.branchVersion ||
        context.ref != r.branchRef || context.head != r.lastCommit) return {staleBranch()};
    return record;
}

BackupResult<BranchSnapshot> BackupEngine::branches(const QString &id)
{
    const auto checked = require(id);
    if (!checked.result.success) return {checked.result};
    const auto git = repository(id);
    const auto branches = git.branches();
    if (!branches.result.success) return {branches.result};
    const auto &r = checked.value;
    return {OperationResult::ok({}), {{id, r.branchRef, r.lastCommit, r.generation, r.branchVersion}, branches.value}};
}

OperationResult BackupEngine::checkBranchTarget(const BranchRequest &request, bool remote)
{
    const auto prefix = remote ? QStringLiteral("refs/remotes/") : QStringLiteral("refs/heads/");
    if (!request.ref.startsWith(prefix) || request.expectedHead.isEmpty()) return staleBranch();
    const auto values = repository(request.context.id).branches();
    if (!values.result.success) return values.result;
    for (const auto &value : values.value)
        if (value.ref == request.ref && value.remoteBranch == remote && value.head == request.expectedHead)
            return OperationResult::ok({});
    return staleBranch();
}

OperationResult BackupEngine::createBranch(const BranchRequest &request)
{
    const auto record = requireBranch(request.context);
    if (!record.result.success) return record.result;
    const auto git = repository(request.context.id);
    auto result = git.validateBranchName(request.name);
    if (!result.success) return result;
    const auto start = git.resolve(request.startCommit);
    if (!start.result.success) return start.result;
    if (!request.upstream.isEmpty())
    {
        BranchRequest target = request; target.ref = request.upstream; target.expectedHead = start.value;
        result = checkBranchTarget(target, true); if (!result.success) return result;
    }
    auto r = record.value;
    r.operation = "create-branch";
    result = m_catalog.save(r); if (!result.success) return result;
    // branch refuses an existing name and preserves the actual HEAD/worktree.
    QStringList args{"branch", "--no-track", "--", request.name, start.value};
    result = GitRepository::outcome(git.run(args), "新建方案失败");
    if (result.success && !request.upstream.isEmpty())
        result = GitRepository::outcome(git.run({"branch", "--set-upstream-to=" + request.upstream, "--", request.name}), "方案已创建，但云端关联失败");
    r.operation.clear(); ++r.branchVersion;
    const auto saved = m_catalog.save(r);
    return !saved.success ? saved : !result.success ? result : OperationResult::ok("方案已创建", request.name);
}

OperationResult BackupEngine::renameBranch(const BranchRequest &request)
{
    const auto record = requireBranch(request.context);
    if (!record.result.success) return record.result;
    auto result = checkBranchTarget(request); if (!result.success) return result;
    const auto git = repository(request.context.id);
    result = git.validateBranchName(request.name); if (!result.success) return result;
    auto r = record.value; r.operation = "rename-branch";
    result = m_catalog.save(r); if (!result.success) return result;
    result = GitRepository::outcome(git.run({"branch", "-m", "--", request.ref.mid(11), request.name}), "改名失败");
    if (!result.success)
    {
        if (git.branchRef().value != record.value.branchRef) return attention(r, result.message);
        const auto reset = m_catalog.save(record.value); if (!reset.success) return reset;
        return result;
    }
    if (r.branchRef == request.ref) r.branchRef = "refs/heads/" + request.name;
    r.operation.clear(); ++r.branchVersion;
    result = m_catalog.save(r);
    return result.success ? OperationResult::ok("方案已改名", "云端分支名称保持原样，原有对应关系已保留。") : result;
}

OperationResult BackupEngine::deleteBranch(const BranchRequest &request)
{
    const auto record = requireBranch(request.context);
    if (!record.result.success) return record.result;
    auto result = checkBranchTarget(request); if (!result.success) return result;
    if (record.value.branchRef == request.ref)
        return OperationResult::fail("无法删除正在使用的方案", "请先切换到其他方案。");
    auto r = record.value; r.operation = "delete-branch";
    result = m_catalog.save(r); if (!result.success) return result;
    result = GitRepository::outcome(repository(r.id).run({"branch", request.force ? "-D" : "-d", "--", request.ref.mid(11)}), "方案未删除");
    r.operation.clear(); ++r.branchVersion;
    const auto saved = m_catalog.save(r); if (!saved.success) return saved;
    return result.success ? OperationResult::ok("方案已删除", "其他方案与云端分支保持原样。") : result;
}

OperationResult BackupEngine::fetchBranches(const BranchContext &context)
{
    const auto checked = requireBranch(context); if (!checked.result.success) return checked.result;
    const auto git = repository(context.id);
    const auto remote = git.remoteName(); if (!remote.result.success) return remote.result;
    const auto result = GitRepository::outcome(git.run({"fetch", "--prune", "--no-tags", "--", remote.value,
        "+refs/heads/*:refs/remotes/" + remote.value + "/*"}, {}, true, 300000), "获取云端方案失败");
    if (!result.success) return result;
    auto record = checked.value; ++record.branchVersion;
    const auto saved = m_catalog.save(record);
    return saved.success ? OperationResult::ok("云端方案已更新", "源文件与当前方案保持原样。") : saved;
}

OperationResult BackupEngine::setBranchUpstream(const BranchRequest &request)
{
    const auto checked = requireBranch(request.context); if (!checked.result.success) return checked.result;
    auto result = checkBranchTarget(request); if (!result.success) return result;
    const auto git = repository(request.context.id);
    if (!request.upstream.isEmpty())
    {
        const auto refs = git.branches(); if (!refs.result.success) return refs.result;
        bool found = false;
        for (const auto &ref : refs.value) if (ref.ref == request.upstream && ref.remoteBranch) found = true;
        if (!found) return staleBranch();
    }
    const auto option = request.upstream.isEmpty() ? QStringLiteral("--unset-upstream") : "--set-upstream-to=" + request.upstream;
    result = GitRepository::outcome(git.run({"branch", option, "--", request.ref.mid(11)}), "云端关联未修改");
    if (!result.success) return result;
    auto record = checked.value; ++record.branchVersion;
    return m_catalog.save(record);
}

OperationResult BackupEngine::deleteRemoteBranch(const BranchRequest &request)
{
    const auto checked = requireBranch(request.context); if (!checked.result.success) return checked.result;
    auto result = checkBranchTarget(request, true); if (!result.success) return result;
    const auto git = repository(request.context.id);
    const auto refs = git.branches(); if (!refs.result.success) return refs.result;
    for (const auto &ref : refs.value)
        if (ref.ref == request.ref && !ref.remote.isEmpty() && ref.remoteRef.startsWith("refs/heads/"))
        {
            result = GitRepository::outcome(git.run({"push", "--no-follow-tags",
                "--force-with-lease=" + ref.remoteRef + ':' + request.expectedHead,
                "--", ref.remote, ':' + ref.remoteRef}, {}, true, 300000), "云端方案未删除");
            if (!result.success) return result;
            // The push may already have removed the tracking ref.
            if (git.resolve(ref.ref).result.success)
            {
                result = GitRepository::outcome(git.run({"update-ref", "-d", ref.ref, request.expectedHead}));
                if (!result.success) return result;
            }
            auto record = checked.value; ++record.branchVersion;
            result = m_catalog.save(record);
            return result.success ? OperationResult::ok("云端方案已删除", "本地方案仍然保留。") : result;
        }
    return staleBranch();
}

BackupResult<HistoryPage> BackupEngine::branchHistory(const QString &id, const HistoryQuery &query)
{
    const auto r = require(id);
    return r.result.success ? repository(id).branchHistory(query) : BackupResult<HistoryPage>{r.result};
}
BackupResult<DiffData> BackupEngine::diffBetween(const QString &id, const QString &oldCommit, const QString &newCommit)
{
    const auto r = require(id);
    return r.result.success ? repository(id).diffBetween(oldCommit, newCommit) : BackupResult<DiffData>{r.result};
}

OperationResult BackupEngine::rebuildBranchesSafe(const QString &id)
{
    const auto git = repository(id);
    const auto refs = git.branches(); if (!refs.result.success) return refs.result;
    const auto current = git.branchRef(); if (!current.result.success) return current.result;
    const auto target = git.targetRef(); if (!target.result.success) return target.result;
    for (const auto &ref : refs.value)
        if ((!ref.remoteBranch && ref.ref != current.value) || (ref.remoteBranch && ref.remoteRef != target.value))
            return OperationResult::fail("多方案备份不能重建", "重建会清空整个仓库的历史，请保留其他方案后再处理。");
    const auto archived = git.tagRefs("refs/zcversionbox-archived/");
    if (!archived.result.success) return archived.result;
    if (!archived.value.isEmpty()) return OperationResult::fail("备份包含保留方案", "重建可能删除保留历史，操作已停止。");
    const auto remote = git.remoteName(); if (!remote.result.success) return remote.result;
    if (git.run({"remote", "get-url", remote.value}).success())
    {
        const auto heads = git.run({"ls-remote", "--heads", "--", remote.value}, {}, true, 300000);
        if (!heads.success()) return OperationResult::fail("暂不能重建", "无法确认云端分支范围，请联网后重试。");
        for (const auto &line : heads.output.split('\n', Qt::SkipEmptyParts))
            if (line.section('\t', 1).trimmed() != target.value)
                return OperationResult::fail("云端包含其他方案", "重建会破坏共享历史，操作已停止。");
    }
    return OperationResult::ok({});
}
