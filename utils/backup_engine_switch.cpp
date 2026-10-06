#include "backup_engine.h"
#include <QFileInfo>
#include <QSet>

namespace
{
QString mapped(const QString &root, const BackupRecord &record)
{ return record.repositoryPath == "." ? root : root + '/' + record.repositoryPath; }
QString contentIdentity(const QString &fingerprint)
{
    auto fields = fingerprint.split('|');
    if (fields.size() >= 3) fields.removeAt(1); // copies need not preserve mtime
    return fields.join('|');
}
OperationResult changedSwitch()
{ return OperationResult::warn("请重新确认切换", "源文件或方案版本已经变化，尚未切换。"); }
}

OperationResult BackupEngine::prepareSwitchFiles(const BackupRecord &record, const QString &target,
    const QString &destination, SourceFingerprint &source, QVector<DiffFile> &changes, bool &savesChanges, QVector<DiffFile> *savedChanges)
{
    auto result = validateSource(record.sourcePath, true); if (!result.success) return result;
    const auto live = repository(record.id);
    result = live.validateMapping(target, record.repositoryPath, record.directory); if (!result.success) return result;
    result = m_dependencies.files->fingerprint(record.sourcePath, record.directory, source, false);
    if (!result.success) return result;
    QTemporaryDir temporary(m_catalog.stagingRoot() + "/switch-XXXXXX");
    if (!temporary.isValid()) return OperationResult::fail("无法准备切换", "无法创建候选目录。");
    const auto repo = temporary.path() + "/repository";
    result = GitRepository::outcome(live.run({"clone", "--no-hardlinks", "--no-checkout", "--", m_catalog.repoPath(record.id), repo}));
    if (!result.success) return result;
    GitRepository candidate(repo, m_dependencies.git, m_cancel);
    for (const auto &config : {QStringList{"config", "user.name", "ZcVersionBox"}, QStringList{"config", "user.email", "backup@zcversionbox.local"}})
    { result = GitRepository::outcome(candidate.run(config)); if (!result.success) return result; }
    // Match repository-local normalization and exclusion rules during the dry run.
    for (const auto *key : {"core.autocrlf", "core.eol", "core.filemode", "core.ignorecase", "core.attributesfile", "core.excludesfile"})
    {
        const auto value = live.run({"config", "--get", key});
        if (value.success()) { result = GitRepository::outcome(candidate.run({"config", key, value.output.trimmed()})); if (!result.success) return result; }
        else if (value.exitCode != 1) return GitRepository::outcome(value);
    }
    for (const auto *name : {"exclude", "attributes"})
    {
        const auto from = m_catalog.repoPath(record.id) + "/.git/info/" + name, to = repo + "/.git/info/" + name;
        if (!BackupFiles::exists(from)) continue;
        result = m_dependencies.files->remove(to); if (!result.success) return result;
        result = m_dependencies.files->copy(from, to); if (!result.success) return result;
    }
    result = GitRepository::outcome(candidate.run({"checkout", "--detach", record.lastCommit})); if (!result.success) return result;
    const auto candidateSource = mapped(repo, record);
    BackupReplacement capture(m_dependencies.files, candidateSource);
    if (!capture.valid()) return OperationResult::fail("无法准备切换", "无法捕获当前内容。");
    SourceFingerprint ignored;
    result = m_dependencies.files->capture(record.sourcePath, record.directory, capture.stagingPath(), ignored);
    if (!result.success) return result;
    result = capture.install(); if (!result.success) return result;
    result = capture.finish(); if (!result.success) return result;
    result = GitRepository::outcome(candidate.run({"add", "--all", "--", record.repositoryPath})); if (!result.success) return result;
    const auto dirty = candidate.run({"diff", "--cached", "--quiet"});
    if (!dirty.started || !dirty.finished || (dirty.exitCode != 0 && dirty.exitCode != 1)) return GitRepository::outcome(dirty);
    savesChanges = dirty.exitCode == 1;
    if (savesChanges)
    {
        result = GitRepository::outcome(candidate.run({"commit", "--only", "-m", "切换前保存", "--", record.repositoryPath}));
        if (!result.success) return result;
    }
    if (savedChanges)
    {
        const auto saved = candidate.diffBetween(record.lastCommit, candidate.head().value);
        if (!saved.result.success) return saved.result;
        *savedChanges = saved.value.files;
    }
    // Ask Git to perform the exact transition, preserving untracked files and refusing ignored-file loss.
    result = GitRepository::outcome(candidate.run({"switch", "--no-overwrite-ignore", "--detach", target}), "无法安全切换方案");
    if (!result.success) return result;
    result = m_dependencies.files->copy(candidateSource, destination); if (!result.success) return result;
    SourceFingerprint after, output;
    result = m_dependencies.files->fingerprint(record.sourcePath, record.directory, after, false); if (!result.success) return result;
    if (source != after) return changedSwitch();
    result = m_dependencies.files->fingerprint(destination, record.directory, output, false); if (!result.success) return result;
    if (!record.directory && !source.isEmpty() && !output.isEmpty())
        output = {{source.firstKey(), output.first()}};
    QSet<QString> names;
    for (auto it = source.cbegin(); it != source.cend(); ++it) names.insert(it.key());
    for (auto it = output.cbegin(); it != output.cend(); ++it) names.insert(it.key());
    auto ordered = names.values(); ordered.sort(); changes.clear();
    for (const auto &name : ordered)
        if (!source.contains(name) || !output.contains(name) || contentIdentity(source[name]) != contentIdentity(output[name]))
            changes.append({!source.contains(name) ? "A" : !output.contains(name) ? "D" : "M",
                            record.directory ? name : QFileInfo(record.sourcePath).fileName(), {}});
    return OperationResult::ok({});
}

BackupResult<PreparedBranchSwitch> BackupEngine::prepareBranchSwitch(const BranchRequest &request)
{
    const auto checked = requireBranch(request.context); if (!checked.result.success) return {checked.result};
    auto result = checkBranchTarget(request); if (!result.success) return {result};
    if (request.ref == checked.value.branchRef) return {OperationResult::info("已在使用此方案", {}), {}};
    QTemporaryDir temp(m_catalog.stagingRoot() + "/switch-preview-XXXXXX");
    if (!temp.isValid()) return {OperationResult::fail("准备失败", "无法创建预览目录")};
    PreparedBranchSwitch prepared{request.context, request.ref, request.expectedHead, {}, {}, false};
    result = prepareSwitchFiles(checked.value, prepared.targetHead, temp.path() + "/source", prepared.sourceFingerprint, prepared.changes, prepared.savesChanges, &prepared.savedChanges);
    if (!result.success) return {result};
    result = checkBranchTarget(request); if (!result.success) return {result};
    const auto verified = requireBranch(request.context); if (!verified.result.success) return {verified.result};
    return {OperationResult::ok({}), prepared};
}

OperationResult BackupEngine::switchBranch(const PreparedBranchSwitch &request)
{
    const auto checked = requireBranch(request.context); if (!checked.result.success) return checked.result;
    BranchRequest target; target.context = request.context; target.ref = request.targetRef; target.expectedHead = request.targetHead;
    auto result = checkBranchTarget(target); if (!result.success) return result;
    if (request.targetRef == checked.value.branchRef) return OperationResult::info("已在使用此方案", {});
    auto git = repository(request.context.id);
    BackupReplacement replacement(m_dependencies.files, checked.value.sourcePath);
    if (!replacement.valid()) return OperationResult::fail("切换失败", "无法准备源文件恢复副本。");
    SourceFingerprint source; QVector<DiffFile> changes; bool savesChanges = false;
    result = prepareSwitchFiles(checked.value, request.targetHead, replacement.stagingPath(), source, changes, savesChanges);
    if (!result.success) return result;
    if (source != request.sourceFingerprint) return changedSwitch();
    result = checkBranchTarget(target); if (!result.success) return result;
    // A single service job owns the lock throughout commit -> switch -> source installation.
    auto backup = prepareBackup(request.context.id);
    if (!backup.result.success) return backup.result;
    result = finishBackup(backup.value, "切换方案前保存当前修改"); if (!result.success) return result;
    const auto before = *m_catalog.find(request.context.id);
    SourceFingerprint current;
    result = m_dependencies.files->fingerprint(before.sourcePath, before.directory, current, false);
    if (!result.success) return result;
    if (current != source) return changedSwitch();
    result = checkBranchTarget(target); if (!result.success) return result;
    if (git.branchRef().value != before.branchRef || git.head().value != before.lastCommit || !git.clean().success) return changedSwitch();
    auto record = before; record.operation = "switch-branch"; record.recoveryPaths = {replacement.recoveryPath()};
    result = m_catalog.save(record); if (!result.success) return result;
    const auto stopped = [&](const OperationResult &failure)
    {
        replacement.preserve();
        m_dependencies.files->cancellation.reset();
        return attention(record, failure.message + "。切换前版本与恢复副本已保留，请检查后再继续。");
    };
    result = GitRepository::outcome(git.run({"switch", "--no-overwrite-ignore", "--", request.targetRef.mid(11)}), "方案未切换");
    if (!result.success)
    {
        if (git.branchRef().value != before.branchRef || git.head().value != before.lastCommit || !git.clean().success) return stopped(result);
        const auto saved = m_catalog.save(before); return saved.success ? result : saved;
    }
    if (git.branchRef().value != request.targetRef || git.head().value != request.targetHead || !git.clean().success) return stopped(changedSwitch());
    result = m_dependencies.files->fingerprint(before.sourcePath, before.directory, current, false);
    if (!result.success || current != source) return stopped(changedSwitch());
    result = replacement.install();
    if (!result.success)
    {
        m_dependencies.files->cancellation.reset();
        const auto rollback = replacement.rollback();
        if (!rollback.success) return stopped(rollback);
        if (git.branchRef().value != request.targetRef || git.head().value != request.targetHead || !git.clean().success) return stopped(result);
        const auto back = git.run({"switch", "--no-overwrite-ignore", "--", before.branchRef.mid(11)}, {}, false);
        if (!back.success() || git.head().value != before.lastCommit) return stopped(result);
        const auto saved = m_catalog.save(before); return saved.success ? result : stopped(saved);
    }
    m_dependencies.files->cancellation.reset();
    result = replacement.verifyInstalled(); if (!result.success) return stopped(result);
    result = m_dependencies.files->fingerprint(before.sourcePath, before.directory, record.fingerprint); if (!result.success) return stopped(result);
    result = replacement.verifyInstalled(); if (!result.success) return stopped(result);
    if (git.branchRef().value != request.targetRef || git.head().value != request.targetHead || !git.clean().success) return stopped(changedSwitch());
    record.branchRef = request.targetRef; record.lastCommit = request.targetHead; ++record.branchVersion;
    return completeReplacement(record, replacement, OperationResult::ok("已切换方案", request.targetRef.mid(11)));
}
