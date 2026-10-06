#include "backup_engine.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>

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
    return {OperationResult::ok({}), {{id, r.branchRef, r.lastCommit, r.generation, r.branchVersion}, branches.value, r.branchesFetchedAt}};
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
    const auto git = repository(request.context.id);
    const auto worktrees = git.run({"worktree", "list", "--porcelain"});
    if (!worktrees.success()) return GitRepository::outcome(worktrees);
    for (const auto &line : worktrees.output.split('\n'))
        if (line == "branch " + request.ref)
            return OperationResult::fail("无法删除正在使用的方案", "请先在使用此方案的工作目录切换到其他方案。");
    if (!request.force)
    {
        auto target = record.value.lastCommit;
        const auto values = git.branches(); if (!values.result.success) return values.result;
        for (const auto &branch : values.value)
            if (branch.ref == request.ref && !branch.upstream.isEmpty())
            {
                const auto upstream = git.resolve(branch.upstream);
                if (upstream.result.success) target = upstream.value;
            }
        const auto merged = git.run({"merge-base", "--is-ancestor", request.expectedHead, target});
        if (!merged.success())
        {
            if (!merged.started || !merged.finished || merged.exitCode != 1) return GitRepository::outcome(merged);
            return OperationResult::warn("方案尚未合并", "Git 的普通删除检查未通过。请先合并，或在确认不再需要独有版本后选择“仍然删除未合并方案”。");
        }
    }
    auto r = record.value; r.operation = "delete-branch";
    result = m_catalog.save(r); if (!result.success) return result;
    // Equivalent merge/checked-out checks to branch -d/-D, with an expected-OID
    // transaction so an external commit cannot be lost between confirmation and deletion.
    result = GitRepository::outcome(git.run({"update-ref", "-d", request.ref, request.expectedHead}), "方案未删除");
    if (result.success)
    {
        const auto config = git.run({"config", "--remove-section", "branch." + request.ref.mid(11)});
        if (!config.success() && config.exitCode != 5) result.warning = "方案已删除，但旧的云端关联配置未能清理：" + config.error;
    }
    r.operation.clear(); ++r.branchVersion;
    const auto saved = m_catalog.save(r); if (!saved.success) return saved;
    if (!result.success) return result;
    auto completed = OperationResult::ok("方案已删除", "其他方案与云端分支保持原样。"); completed.warning = result.warning; return completed;
}

OperationResult BackupEngine::fetchBranches(const BranchContext &context)
{
    const auto checked = requireBranch(context); if (!checked.result.success) return checked.result;
    const auto git = repository(context.id);
    const auto remote = git.remoteName(); if (!remote.result.success) return remote.result;
    const auto result = GitRepository::outcome(git.run({"fetch", "--prune", "--no-tags", "--", remote.value,
        "+refs/heads/*:refs/remotes/" + remote.value + "/*"}, {}, true, 300000), "获取云端方案失败");
    if (!result.success) return result;
    auto record = checked.value; ++record.branchVersion; record.branchesFetchedAt = QDateTime::currentDateTimeUtc();
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
            if (request.endpoint.isEmpty() || request.endpoint != ref.endpoint) return staleBranch();
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
    const auto remotes = git.run({"remote"}); if (!remotes.success()) return GitRepository::outcome(remotes);
    QSet<QString> addresses;
    for (const auto &remote : remotes.output.split('\n', Qt::SkipEmptyParts))
    {
        for (const bool push : {false, true})
        {
            QStringList args{"remote", "get-url", "--all"}; if (push) args << "--push"; args << remote;
            const auto urls = git.run(args); if (!urls.success()) return GitRepository::outcome(urls);
            for (const auto &url : urls.output.split('\n', Qt::SkipEmptyParts)) addresses.insert(url);
        }
    }
    for (const auto &address : addresses)
    {
        const auto heads = git.run({"ls-remote", "--heads", "--", address}, {}, true, 300000);
        if (!heads.success()) return OperationResult::fail("暂不能重建", "无法确认云端分支范围，请联网后重试。");
        for (const auto &line : heads.output.split('\n', Qt::SkipEmptyParts))
            if (line.section('\t', 1).trimmed() != target.value)
                return OperationResult::fail("云端包含其他方案", "重建会破坏共享历史，操作已停止。");
    }
    return OperationResult::ok({});
}

OperationResult BackupEngine::uploadBranch(const BranchRequest &request)
{
    const auto checked = requireBranch(request.context); if (!checked.result.success) return checked.result;
    auto result = checkBranchTarget(request); if (!result.success) return result;
    const auto git = repository(request.context.id);
    const auto branches = git.branches(); if (!branches.result.success) return branches.result;
    BranchInfo branch;
    for (const auto &b : branches.value) if (b.ref == request.ref) branch = b;
    if (request.endpoint.isEmpty() || branch.endpoint != request.endpoint) return OperationResult::warn("请刷新云端配置", "云端地址尚未配置或已发生变化。");
    const auto remote = branch.remote.isEmpty() ? git.remoteName().value : branch.remote;
    const bool publish = !request.name.isEmpty() || branch.upstream.isEmpty();
    const auto name = request.name.isEmpty() ? branch.name : request.name;
    result = git.validateBranchName(name); if (!result.success) return result;
    const auto target = publish ? "refs/heads/" + name : branch.remoteRef;
    if (!target.startsWith("refs/heads/")) return staleBranch();
    if (publish)
    {
        // Check the push endpoint, which may differ from the fetch URL.
        const auto addresses = git.run({"remote", "get-url", "--push", "--all", remote}); if (!addresses.success()) return GitRepository::outcome(addresses);
        for (const auto &address : addresses.output.split('\n', Qt::SkipEmptyParts))
        {
            const auto existing = git.run({"ls-remote", "--heads", "--", address, target}, {}, true, 300000);
            if (!existing.success()) return GitRepository::outcome(existing);
            if (!existing.output.trimmed().isEmpty()) return OperationResult::warn("同名云端方案已存在", "请先获取云端方案并设置对应关系，再上传；不会覆盖已有方案。");
        }
    }
    if (git.remoteEndpoint(remote).value != request.endpoint) return staleBranch();
    QStringList args{"push", "--no-follow-tags"};
    if (publish) args << "--force-with-lease=" + target + ':'; // absent-only creation
    args << "--" << remote << request.expectedHead + ':' + target;
    result = GitRepository::outcome(git.run(args, {}, true, 300000), "上传方案失败");
    if (!result.success) return result;
    if (git.resolve(request.ref).value != request.expectedHead || git.remoteEndpoint(remote).value != request.endpoint)
        return OperationResult::warn("所选版本已上传", "本地方案或云端配置随后发生变化，请刷新后核对对应关系。");
    auto record = checked.value; record.operation = "branch-upstream";
    result = m_catalog.save(record); if (!result.success) return result;
    result = GitRepository::outcome(git.run({"config", "branch." + branch.name + ".remote", remote})); if (!result.success) return result;
    result = GitRepository::outcome(git.run({"config", "branch." + branch.name + ".merge", target})); if (!result.success) return result;
    const auto endpoint = QString::fromLatin1(QCryptographicHash::hash(request.endpoint.toUtf8() + '\0' + target.toUtf8(), QCryptographicHash::Sha256).toHex());
    record.branchRemotes[endpoint].lastUploadedHead = request.expectedHead;
    record.operation.clear(); ++record.branchVersion;
    result = m_catalog.save(record);
    if (!result.success) return result;
    auto completed = OperationResult::ok("方案已上传", QString("%1 → %2/%3").arg(branch.name, remote, target.mid(11)));
    if (git.remoteName().value == remote)
    {
        const auto tags = syncTags(record.id, true);
        if (!tags.success) completed.warning = "版本已上传，里程碑尚未同步：" + tags.message;
        else completed.warning = tags.warning;
    }
    return completed;
}
