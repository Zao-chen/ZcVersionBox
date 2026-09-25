#include "backup_engine.h"
#include <QDirIterator>
#include <QFile>
#include <QUuid>
#include <algorithm>

namespace
{
OperationResult missing() { return OperationResult::fail("操作失败", "追踪对象不存在"); }
OperationResult paused(const BackupRecord &r) { return OperationResult::warn("自动备份已暂停", backupStateText(r.state) + "。" + r.stateDetail); }
QString contentPath(const QString &repo, const BackupRecord &r) { return r.repositoryPath == "." ? repo : QDir(repo).filePath(r.repositoryPath); }
QPair<int, qint64> pathStats(const QString &path, bool excludeGit = false)
{
    if (BackupFiles::isLink(path) || !QFileInfo::exists(path))
        return {};
    if (QFileInfo(path).isFile())
        return {1, QFileInfo(path).size()};
    QPair<int, qint64> result;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        if (!excludeGit || !it.filePath().contains("/.git/"))
        {
            ++result.first;
            result.second += it.fileInfo().size();
        }
    }
    return result;
}
} // namespace
BackupEngine::BackupEngine(AppPaths paths, BackupDependencies dependencies) : m_catalog(std::move(paths)), m_dependencies(std::move(dependencies))
{
    m_catalog.allowSave = m_dependencies.allowSave;
}
BackupEngine::~BackupEngine()
{
    if (m_pending)
        finishBackup(m_pending, {}, true);
    m_dependencies.files->cancellation.reset();
    for (const auto &path : m_previews)
        m_dependencies.files->remove(path);
}
OperationResult BackupEngine::begin(std::shared_ptr<std::atomic_bool> cancellation)
{
    m_cancel = std::move(cancellation);
    m_dependencies.files->cancellation = m_cancel;
    const auto root = m_catalog.paths().backupRoot;
    for (const auto &path : {root, root + "/items", root + "/staging"})
        if (BackupFiles::hasLinkedAncestor(path))
            return OperationResult::fail("操作已暂停", "备份存储路径已被链接替换：" + path);
    if (!QDir().mkpath(root))
        return OperationResult::fail("操作失败", "无法创建备份存储目录");
    m_lock = std::make_unique<QLockFile>(m_catalog.lockPath());
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0))
    {
        m_lock.reset();
        return OperationResult::warn("备份存储忙", "另一个应用进程正在操作备份，请稍后重试");
    }
    auto result = m_catalog.load();
    for (const auto &path : {root + "/items", root + "/staging"})
        if (!QDir().mkpath(path))
            return OperationResult::fail("操作失败", "无法创建备份存储目录：" + path);
    for (const auto &r : m_catalog.records())
        m_generations[r.id] = qMax(m_generations.value(r.id), r.generation);
    return result;
}
void BackupEngine::end()
{
    m_lock.reset();
    m_cancel.reset();
    m_dependencies.files->cancellation.reset();
}
QVector<BackupRecord> BackupEngine::records() const { return m_catalog.records().values().toVector(); }
OperationResult BackupEngine::reload()
{
    QStringList warnings;
    for (const auto &record : records())
    {
        if (record.state != BackupSyncState::Tracking)
            continue;
        const auto checked = require(record.id, true);
        if (!checked.result.success)
            warnings.append(QDir::toNativeSeparators(record.sourcePath) + "：" + checked.result.message);
    }
    auto result = OperationResult::ok("备份存储检查");
    result.warning = warnings.join('\n');
    return result;
}
GitRepository BackupEngine::repository(const QString &id) const { return GitRepository(m_catalog.repoPath(id), m_dependencies.git, m_cancel); }
QString BackupEngine::newId(const QString &source) const { return m_dependencies.makeId ? m_dependencies.makeId(source) : QUuid::createUuid().toString(QUuid::WithoutBraces); }
OperationResult BackupEngine::validateSource(const QString &path, bool mustExist) const
{
    if (path.isEmpty() || QFileInfo(path).fileName().isEmpty() || BackupFiles::hasLinkedAncestor(path))
        return OperationResult::fail("路径无效", "请选择不经过符号链接或目录联接的普通文件或文件夹");
    if (BackupFiles::isGitMetadataPath(path))
        return OperationResult::fail("路径无效", "不能选择 .git 元数据目录、Git 指针文件或其内部路径；请选择仓库中的普通文件或文件夹");
    if (BackupFiles::overlaps(path, m_catalog.paths().backupRoot))
        return OperationResult::fail("路径无效", "源位置与备份存储不能互相包含，以免递归备份或覆盖仓库");
    if (mustExist && (!QFileInfo::exists(path) || (!QFileInfo(path).isFile() && !QFileInfo(path).isDir())))
        return OperationResult::fail("路径无效", "源文件或文件夹不存在");
    return OperationResult::ok({});
}
OperationResult BackupEngine::attention(BackupRecord r, const QString &reason)
{
    r.state = BackupSyncState::NeedsAttention;
    r.stateDetail = reason;
    r.operation.clear();
    const auto saved = m_catalog.save(r);
    auto result = OperationResult::fail("自动备份已暂停", reason);
    if (!saved.success)
        result.warning = saved.message;
    return result;
}
BackupResult<BackupRecord> BackupEngine::require(const QString &id, bool writing, bool allowPending)
{
    const auto *found = m_catalog.find(id);
    if (!found)
        return {missing()};
    auto r = *found;
    if (BackupFiles::isLink(m_catalog.itemPath(id)))
        return {OperationResult::fail("操作已暂停", "追踪记录目录已被链接替换")};
    auto git = repository(id);
    auto checked = git.validate();
    if (!checked.success)
        return {writing ? attention(r, checked.message) : checked};
    if (!writing)
        return {OperationResult::ok({}), r};
    if (r.state == BackupSyncState::NeedsAttention || (r.state == BackupSyncState::RemotePending && !allowPending))
        return {paused(r)};
    const auto branch = git.branch();
    if (!branch.result.success)
        return {attention(r, branch.result.message)};
    checked = git.clean();
    if (!checked.success)
        return {attention(r, checked.message)};
    const auto head = git.head();
    if (!head.result.success)
        return {head.result};
    const auto expected = r.state == BackupSyncState::RemotePending ? r.pendingCommit : r.lastCommit;
    if (head.value != expected)
    {
        const auto difference = git.run({"diff", "--quiet", expected, head.value, "--", r.repositoryPath});
        if (r.state == BackupSyncState::Tracking && difference.success())
        {
            r.lastCommit = head.value; // Message-only external amendments leave the source baseline unchanged.
            auto saved = m_catalog.save(r);
            if (!saved.success)
                return {saved};
        }
        else
            return {attention(r, "同步仓库的提交在应用外发生变化，请先检查源位置与仓库内容")};
    }
    return {OperationResult::ok({}), r};
}
OperationResult BackupEngine::addLocal(const QString &source)
{
    auto valid = validateSource(source, true);
    if (!valid.success)
        return valid;
    const auto normalized = BackupCatalog::normalizedSource(source);
    if (!m_catalog.idForSource(normalized).isEmpty())
        return OperationResult::info("已在追踪", "该源位置已有追踪记录");
    BackupRecord r;
    r.id = newId(normalized);
    r.sourcePath = normalized;
    r.repositoryPath = QFileInfo(normalized).fileName();
    if (!BackupCatalog::validRepositoryPath(r.repositoryPath))
        return OperationResult::fail("添加失败", "该名称不能作为同步仓库内的普通内容路径");
    r.directory = QFileInfo(normalized).isDir();
    r.generation = m_generations.value(r.id) + 1;
    if (!BackupCatalog::validId(r.id) || BackupFiles::exists(m_catalog.itemPath(r.id)))
        return OperationResult::fail("添加失败", "追踪标识已存在，请重新添加");
    QDir().mkpath(m_catalog.stagingRoot());
    QTemporaryDir stage(m_catalog.stagingRoot() + "/add-XXXXXX");
    if (!stage.isValid())
        return OperationResult::fail("添加失败", "无法创建候选仓库");
    auto copied = m_dependencies.files->capture(normalized, r.directory, stage.path() + '/' + r.repositoryPath, r.fingerprint);
    if (!copied.success)
        return copied;
    GitRepository git(stage.path(), m_dependencies.git, m_cancel);
    auto initialized = git.initialize();
    if (!initialized.success)
        return initialized;
    auto command = git.run({"add", "--all", "--", "."});
    if (!command.success())
        return GitRepository::outcome(command);
    command = git.run({"commit", "--allow-empty", "-m", "Initial backup"});
    if (!command.success())
        return GitRepository::outcome(command);
    if (!r.directory && !git.run({"cat-file", "-e", "HEAD:" + r.repositoryPath}).success())
        return OperationResult::fail("添加失败", "该文件被 Git 忽略规则排除，没有可保存的文件版本");
    const auto initial = git.head();
    if (!initial.result.success)
        return initial.result;
    r.lastCommit = initial.value;
    if (!QDir().mkpath(m_catalog.itemPath(r.id)))
        return OperationResult::fail("添加失败", "无法创建追踪目录");
    const auto installed = m_dependencies.files->rename(stage.path(), m_catalog.repoPath(r.id));
    if (!installed.success)
        return installed;
    const auto saved = m_catalog.save(r);
    if (!saved.success)
    {
        m_dependencies.files->rename(m_catalog.repoPath(r.id), stage.path());
        QDir().rmdir(m_catalog.itemPath(r.id));
        return saved;
    }
    m_generations[r.id] = r.generation;
    return OperationResult::ok("添加成功", "已将文件添加至版本控制");
}
BackupResult<bool> BackupEngine::changed(const QString &id)
{
    auto r = require(id);
    if (!r.result.success)
        return {r.result};
    if (r.value.state == BackupSyncState::Tracking)
    {
        r = require(id, true);
        if (!r.result.success)
            return {r.result};
    }
    SourceFingerprint now;
    const auto result = m_dependencies.files->fingerprint(r.value.sourcePath, r.value.directory, now);
    if (!result.success)
        return {result};
    return {OperationResult::ok({}), now != r.value.fingerprint && r.value.state == BackupSyncState::Tracking};
}
BackupResult<std::shared_ptr<PendingBackup>> BackupEngine::prepareBackup(const QString &id, bool changedOnly, const RestoreRequest *resolution)
{
    auto checked = require(id, true, resolution != nullptr);
    if (!checked.result.success)
        return {checked.result};
    auto r = checked.value;
    if (resolution)
    {
        if (!resolution->pulledVersion || resolution->id != id)
            return {OperationResult::warn("操作已取消", "拉取确认上下文无效")};
        const auto verified = verifyRequest(*resolution, r);
        if (!verified.success)
            return {verified};
    }
    SourceFingerprint now;
    auto scanned = m_dependencies.files->fingerprint(r.sourcePath, r.directory, now);
    if (!scanned.success)
        return {scanned};
    if (changedOnly && now == r.fingerprint)
        return {OperationResult::ok({}), nullptr};
    auto work = std::make_shared<PendingBackup>();
    work->before = r;
    const auto initialHead = repository(id).head();
    if (!initialHead.result.success)
        return {initialHead.result};
    work->head = initialHead.value;
    work->replacement = std::make_shared<BackupReplacement>(m_dependencies.files, contentPath(m_catalog.repoPath(id), r), m_catalog.itemPath(id));
    if (!work->replacement->valid())
        return {OperationResult::fail("备份失败", "无法创建候选副本")};
    auto result = m_dependencies.files->capture(r.sourcePath, r.directory, work->replacement->stagingPath(), work->fingerprint);
    if (!result.success)
        return {result};
    if (resolution)
    {
        result = verifyRequest(*resolution, r);
        if (!result.success)
            return {result};
    }
    auto git = repository(id);
    result = git.clean();
    if (!result.success || git.head().value != work->head)
        return {OperationResult::warn("操作已取消", "准备副本期间仓库发生变化")};
    r.operation = "backup";
    r.recoveryPaths = {work->replacement->recoveryPath()};
    result = m_catalog.save(r);
    if (!result.success)
        return {result};
    m_pending = work;
    result = work->replacement->install();
    if (!result.success)
        return {abortBackup(work, result)};
    const auto beforeStage = git.stagedState();
    if (!beforeStage.result.success || !beforeStage.value.isEmpty())
        return {abortBackup(work, OperationResult::fail("备份已暂停", "替换期间暂存区发生外部变化，已保留现场"))};
    auto staged = git.run({"add", "--all", "--", r.repositoryPath});
    if (!staged.success())
        return {abortBackup(work, GitRepository::outcome(staged))};
    auto index = git.stagedState(r.repositoryPath);
    if (!index.result.success)
        return {abortBackup(work, index.result)};
    work->indexState = index.value;
    const auto difference = git.run({"diff", "--cached", "--quiet"});
    if (!difference.started || !difference.finished || (difference.exitCode != 0 && difference.exitCode != 1))
        return {abortBackup(work, GitRepository::outcome(difference))};
    work->changed = difference.exitCode == 1;
    if (work->changed)
    {
        const auto diff = git.run({"diff", "--cached", "--no-ext-diff", "--no-textconv", "--unified=0"});
        if (diff.success())
            work->diff = diff.output.left(128 * 1024);
    }
    return {OperationResult::ok({}), work};
}
OperationResult BackupEngine::abortBackup(const std::shared_ptr<PendingBackup> &work, OperationResult failure)
{
    const auto retained = work;
    m_pending.reset();
    m_dependencies.files->cancellation.reset();
    auto git = GitRepository(m_catalog.repoPath(work->before.id), m_dependencies.git);
    const auto head = git.head();
    const auto index = git.stagedState(work->before.repositoryPath);
    if (!head.result.success || head.value != work->head || !index.result.success || index.value != work->indexState)
    {
        work->replacement->preserve();
        auto r = work->before;
        r.recoveryPaths = {work->replacement->recoveryPath()};
        return attention(r, failure.message + "；仓库状态不确定，已保留副本：" + work->replacement->recoveryPath());
    }
    auto rolled = work->replacement->rollback();
    if (!rolled.success)
    {
        auto r = work->before;
        r.recoveryPaths = {work->replacement->recoveryPath()};
        return attention(r, failure.message + "；回滚未完成：" + rolled.message);
    }
    const auto reset = git.run({"reset", "--quiet", "HEAD", "--", "."});
    if (!reset.success())
    {
        auto r = work->before;
        return attention(r, failure.message + "；文件已恢复，但暂存区未能恢复：" + reset.error);
    }
    auto saved = m_catalog.save(work->before);
    if (!saved.success)
        failure.warning = saved.message;
    return failure;
}
OperationResult BackupEngine::finishBackup(std::shared_ptr<PendingBackup> work, const QString &message, bool cancelled)
{
    const auto retained = work;
    m_pending.reset();
    if (!work)
        return OperationResult::ok({});
    if (cancelled || (m_cancel && m_cancel->load()))
        return abortBackup(work, OperationResult::warn("备份已取消", "已保留上一次同步副本"));
    auto git = repository(work->before.id);
    auto index = git.stagedState(work->before.repositoryPath);
    if (git.head().value != work->head || !index.result.success || index.value != work->indexState || !git.run({"diff", "--quiet"}).success() || !work->replacement->verifyInstalled().success)
    {
        work->replacement->preserve();
        auto r = work->before;
        r.recoveryPaths = {work->replacement->recoveryPath()};
        return attention(r, "准备提交期间检测到外部 Git 修改，已保留副本：" + work->replacement->recoveryPath());
    }
    if (work->changed)
    {
        // Keep an unrelated path staged by an external process out of the commit,
        // including a change arriving after the last pre-commit check.
        auto committed = git.run({"commit", "--only", "-F", "-", "--", work->before.repositoryPath}, (message.trimmed().isEmpty() ? "Auto backup - " + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") : message.trimmed()).toUtf8());
        if (!committed.success())
            return abortBackup(work, GitRepository::outcome(committed, "自动备份失败"));
    }
    const auto remaining = git.stagedState();
    if (!remaining.result.success || !remaining.value.isEmpty() || !git.clean().success || !work->replacement->verifyInstalled().success)
    {
        work->replacement->preserve();
        auto r = work->before;
        r.recoveryPaths = {work->replacement->recoveryPath()};
        return attention(r, "提交期间发现外部修改或无法确认内容，未推进成功基线；副本保留在：" + work->replacement->recoveryPath());
    }
    auto r = work->before;
    const auto head = git.head();
    if (!head.result.success)
    {
        work->replacement->preserve();
        auto failure = head.result;
        failure.warning = "无法确认提交结果，自动备份暂停；副本：" + work->replacement->recoveryPath();
        return failure;
    }
    r.lastCommit = head.value;
    r.fingerprint = work->fingerprint;
    r.state = BackupSyncState::Tracking;
    r.stateDetail.clear();
    r.pendingCommit.clear();
    r.operation.clear();
    r.recoveryPaths.clear();
    auto saved = m_catalog.save(r);
    if (!saved.success)
    {
        work->replacement->preserve();
        saved.warning = "文件与提交已保留；追踪状态未能确认，自动备份暂停。副本：" + work->replacement->recoveryPath();
        return saved;
    }
    m_dependencies.files->cancellation.reset();
    const auto cleaned = work->replacement->finish();
    auto result = OperationResult::ok({});
    if (work->before.state == BackupSyncState::RemotePending)
        result = OperationResult::ok("已恢复自动备份", "已保留源内容，拉取版本仍保留在历史中");
    if (!cleaned.success)
        result.warning = cleaned.message;
    return result;
}

BackupResult<BackupStats> BackupEngine::statistics(const QString &id)
{
    auto r = require(id);
    if (!r.result.success)
        return {r.result};
    BackupStats stats;
    stats.sourceState = !QFileInfo::exists(r.value.sourcePath) ? "缺失" : QFileInfo(r.value.sourcePath).isDir() ? "文件夹"
                                                                                                                : "文件";
    stats.syncState = r.value.state;
    stats.syncDetail = r.value.stateDetail;
    stats.pendingCommit = r.value.pendingCommit;
    auto files = pathStats(contentPath(m_catalog.repoPath(id), r.value), true);
    stats.fileCount = files.first;
    stats.fileSize = files.second;
    stats.cacheSize = pathStats(m_catalog.repoPath(id) + "/.git/objects").second;
    const auto git = repository(id);
    auto count = git.run({"rev-list", "--count", "HEAD"});
    if (!count.success())
        return {GitRepository::outcome(count)};
    stats.versionCount = count.output.trimmed().toInt();
    const auto remoteName = git.remoteName();
    if (remoteName.result.success)
    {
        auto remote = git.run({"remote", "get-url", remoteName.value});
        if (remote.success())
            stats.remoteUrl = remote.output.trimmed();
    }
    return {OperationResult::ok({}), stats};
}
BackupResult<QVector<Revision>> BackupEngine::history(const QString &id)
{
    auto r = require(id);
    return r.result.success ? repository(id).history() : BackupResult<QVector<Revision>>{r.result};
}
BackupResult<DiffData> BackupEngine::diff(const QString &id, const QString &commit)
{
    auto r = require(id);
    return r.result.success ? repository(id).diff(commit) : BackupResult<DiffData>{r.result};
}
BackupResult<QString> BackupEngine::diffText(const QString &id, const DiffData &data, const QString &file)
{
    auto r = require(id);
    return r.result.success ? repository(id).diffText(data, file) : BackupResult<QString>{r.result};
}
OperationResult BackupEngine::preview(const QString &id, const QString &commit)
{
    auto r = require(id);
    if (!r.result.success)
        return r.result;
    QTemporaryDir temporary(QDir(QDir::tempPath()).canonicalPath() + "/ZcVersionBoxPreview-XXXXXX");
    if (!temporary.isValid())
        return OperationResult::fail("预览失败", "无法创建预览目录");
    auto result = repository(id).exportRevision(commit, ".", temporary.path() + "/contents", *m_dependencies.files);
    if (!result.success)
        return result;
    auto permissions = m_dependencies.files->readOnly(temporary.path());
    if (!permissions.success)
        result.warning += permissions.message;
    result.path = temporary.path() + "/contents";
    m_previews.append(temporary.path());
    temporary.setAutoRemove(false);
    return result;
}
BackupResult<RestoreRequest> BackupEngine::prepareRestore(const QString &id, const QString &commit, bool pulledVersion)
{
    auto r = require(id, true, pulledVersion);
    if (!r.result.success)
        return {r.result};
    if (pulledVersion && r.value.state != BackupSyncState::RemotePending)
        return {OperationResult::warn("操作已取消", "没有待处理的拉取版本")};
    const auto resolved = repository(id).resolve(pulledVersion ? r.value.pendingCommit : commit);
    if (!resolved.result.success)
        return {resolved.result};
    const auto mapping = repository(id).validateMapping(resolved.value, r.value.repositoryPath, r.value.directory);
    if (!mapping.success)
        return {mapping};
    RestoreRequest request{id, resolved.value, r.value.generation, {}, BackupFiles::exists(r.value.sourcePath), pulledVersion};
    if (request.sourceExists)
    {
        auto scan = m_dependencies.files->fingerprint(r.value.sourcePath, r.value.directory, request.sourceFingerprint, false);
        if (!scan.success)
            return {scan};
    }
    return {OperationResult::ok({}), request};
}
OperationResult BackupEngine::verifyRequest(const RestoreRequest &request, BackupRecord &record)
{
    auto r = require(request.id, true, request.pulledVersion);
    if (!r.result.success)
        return r.result;
    record = r.value;
    if (record.generation != request.generation || (request.pulledVersion && (record.state != BackupSyncState::RemotePending || request.commit != record.pendingCommit)))
        return OperationResult::warn("操作已取消", "仓库或拉取版本已变化，请重新确认");
    const auto mapping = repository(request.id).validateMapping(request.commit, record.repositoryPath, record.directory);
    if (!mapping.success)
        return mapping;
    if (request.sourceExists != BackupFiles::exists(record.sourcePath))
        return OperationResult::warn("操作已取消", "源位置在确认期间发生变化，请重新确认");
    if (request.sourceExists)
    {
        SourceFingerprint current;
        auto scan = m_dependencies.files->fingerprint(record.sourcePath, record.directory, current, false);
        if (!scan.success)
            return scan;
        if (current != request.sourceFingerprint)
            return OperationResult::warn("操作已取消", "源内容在确认期间发生变化，请重新确认");
    }
    return OperationResult::ok({});
}
OperationResult BackupEngine::completeReplacement(BackupRecord record, BackupReplacement &replacement, OperationResult result)
{
    record.operation.clear();
    record.recoveryPaths.clear();
    auto saved = m_catalog.save(record);
    if (!saved.success)
    {
        replacement.preserve();
        saved.warning = "替换已完成，但状态未能保存；自动备份暂停。原内容保留在：" + replacement.recoveryPath();
        return saved;
    }
    m_dependencies.files->cancellation.reset();
    auto cleaned = replacement.finish();
    if (!cleaned.success)
        result.warning += cleaned.message;
    return result;
}
OperationResult BackupEngine::restore(const RestoreRequest &request)
{
    BackupRecord r;
    auto verified = verifyRequest(request, r);
    if (!verified.success)
        return verified;
    auto validated = validateSource(r.sourcePath, false);
    if (!validated.success)
        return validated;
    BackupReplacement replacement(m_dependencies.files, r.sourcePath);
    if (!replacement.valid())
        return OperationResult::fail("还原失败", "无法准备源位置的恢复副本");
    auto exported = repository(r.id).exportRevision(request.commit, r.repositoryPath, replacement.stagingPath(), *m_dependencies.files);
    if (!exported.success)
        return exported;
    if (QFileInfo(replacement.stagingPath()).isDir() != r.directory)
        return OperationResult::fail("还原失败", "所选版本的追踪内容类型与记录不匹配");
    verified = verifyRequest(request, r);
    if (!verified.success)
        return verified;
    const auto before = r;
    r.operation = request.pulledVersion ? "apply-pull" : "restore";
    r.recoveryPaths = {replacement.recoveryPath()};
    auto saved = m_catalog.save(r);
    if (!saved.success)
        return saved;
    auto installed = replacement.install();
    if (!installed.success)
    {
        m_dependencies.files->cancellation.reset();
        const auto rolled = replacement.rollback();
        if (!rolled.success)
            return attention(r, installed.message + "；" + rolled.message);
        const auto reset = m_catalog.save(before);
        if (!reset.success)
            installed.warning = reset.message;
        return installed;
    }
    SourceFingerprint restored;
    if (request.pulledVersion)
    {
        m_dependencies.files->cancellation.reset();
        auto scan = m_dependencies.files->fingerprint(r.sourcePath, r.directory, restored);
        if (!scan.success)
        {
            replacement.preserve();
            return attention(r, scan.message);
        }
    }
    const auto intact = replacement.verifyInstalled();
    if (!intact.success)
    {
        replacement.preserve();
        return attention(r, intact.message);
    }
    if (request.pulledVersion)
    {
        r.fingerprint = std::move(restored);
        r.lastCommit = r.pendingCommit;
        r.pendingCommit.clear();
        r.state = BackupSyncState::Tracking;
        r.stateDetail.clear();
    }
    return completeReplacement(r, replacement, OperationResult::ok(request.pulledVersion ? "已恢复自动备份" : "还原成功", request.pulledVersion ? "已将拉取版本应用到源位置" : "已恢复到版本 " + request.commit.left(12)));
}
OperationResult BackupEngine::editMessage(const QString &id, const QString &commit, const QString &message)
{
    auto checked = require(id);
    if (!checked.result.success)
        return checked.result;
    auto r = checked.value;
    if (r.state != BackupSyncState::Tracking)
        return paused(r);
    if (message.trimmed().isEmpty())
        return OperationResult::fail("无法编辑", "提交说明不能为空");
    auto git = repository(id);
    const auto selected = git.resolve(commit), current = git.head();
    if (!selected.result.success)
        return selected.result;
    if (!current.result.success)
        return current.result;
    if (selected.value != current.value)
        return OperationResult::fail("无法编辑", "只能编辑最新的提交说明");
    if (current.value != r.lastCommit && !git.run({"diff", "--quiet", r.lastCommit, current.value, "--", r.repositoryPath}).success())
        return attention(r, "同步仓库在应用外发生内容变化，请先检查后再编辑说明");
    const auto before = r;
    r.operation = "edit-message";
    const auto saved = m_catalog.save(r);
    if (!saved.success)
        return saved;
    // --only amends the HEAD tree, leaving any staged content out of the amendment.
    const auto amended = git.run({"commit", "--amend", "--only", "-F", "-"}, message.trimmed().toUtf8());
    if (!amended.success())
    {
        if (git.head().value == current.value)
            m_catalog.save(before);
        else
            return attention(r, "修改说明的结果不确定，请检查仓库");
        return GitRepository::outcome(amended, "保存失败");
    }
    const auto amendedHead = git.head();
    if (!amendedHead.result.success)
        return attention(r, "修改说明后无法确认提交，请检查仓库");
    r.lastCommit = amendedHead.value;
    r.operation.clear();
    auto recorded = m_catalog.save(r);
    return recorded.success ? OperationResult::ok("已保存", "提交说明已更新") : recorded;
}
OperationResult BackupEngine::setRemote(const QString &id, const QString &url)
{
    auto r = require(id);
    if (!r.result.success)
        return r.result;
    if (url.trimmed().isEmpty() || url.trimmed().startsWith('-'))
        return OperationResult::fail("保存失败", "云端仓库地址无效");
    auto git = repository(id);
    const auto remote = git.remoteName();
    if (!remote.result.success)
        return remote.result;
    auto existing = git.run({"remote", "get-url", remote.value});
    auto result = GitRepository::outcome(git.run({"remote", existing.success() ? "set-url" : "add", remote.value, url.trimmed()}));
    return result.success ? OperationResult::ok("云端地址已保存", url.trimmed(), 2000) : result;
}
OperationResult BackupEngine::removeRemote(const QString &id)
{
    auto r = require(id);
    if (!r.result.success)
        return r.result;
    auto git = repository(id);
    const auto remote = git.remoteName();
    if (!remote.result.success)
        return remote.result;
    if (git.run({"remote", "get-url", remote.value}).success())
    {
        const auto removed = git.run({"remote", "remove", remote.value});
        if (!removed.success())
            return GitRepository::outcome(removed);
    }
    return OperationResult::ok("已关闭云端同步", {}, 2000);
}
OperationResult BackupEngine::synchronize(const QString &id, bool push)
{
    auto checked = require(id, !push);
    if (!checked.result.success)
        return checked.result;
    auto git = repository(id);
    if (push)
    {
        if (checked.value.state == BackupSyncState::NeedsAttention)
            return paused(checked.value);
        const auto pushed = git.push();
        return pushed.success ? OperationResult::ok("上传完成", "已提交的历史已上传到云端", 2000) : pushed;
    }
    auto before = checked.value;
    const auto previous = git.head();
    if (!previous.result.success)
        return previous.result;
    auto r = before;
    r.state = BackupSyncState::RemotePending;
    r.pendingCommit = previous.value;
    r.stateDetail = "正在拉取，源位置尚未修改";
    r.operation = "pull";
    auto saved = m_catalog.save(r);
    if (!saved.success)
        return saved; // Never change HEAD without a durable stop condition.
    auto pulled = git.pull();
    const auto current = git.head();
    if (!pulled.success)
    {
        if (current.result.success && current.value == previous.value && git.clean().success)
        {
            const auto reset = m_catalog.save(before);
            if (!reset.success)
                pulled.warning = reset.message;
            return pulled;
        }
        return attention(r, "拉取未正常完成，仓库状态需要检查：" + pulled.message);
    }
    if (!current.result.success)
        return attention(r, "拉取后无法确认仓库提交");
    const auto difference = git.run({"diff", "--quiet", previous.value, current.value, "--", r.repositoryPath});
    if (!difference.started || !difference.finished || (difference.exitCode != 0 && difference.exitCode != 1))
        return attention(r, "无法确认拉取后的受管理内容");
    r.operation.clear();
    if (difference.exitCode == 0)
    {
        r.state = BackupSyncState::Tracking;
        r.stateDetail.clear();
        r.pendingCommit.clear();
        r.lastCommit = current.value;
    }
    else
    {
        r.pendingCommit = current.value;
        r.stateDetail = "源位置保持原样。请选择应用拉取版本，或保留源内容创建新版本；自动备份已暂停。";
    }
    saved = m_catalog.save(r);
    if (!saved.success)
        return saved;
    return r.state == BackupSyncState::RemotePending ? OperationResult::info("已拉取，源位置待处理", r.stateDetail, 6000)
                                                     : OperationResult::ok("已同步", "受管理内容未变化，继续自动备份", 2000);
}
OperationResult BackupEngine::removeBackup(const QString &id)
{
    const auto *r = m_catalog.find(id);
    if (!r)
        return missing();
    const auto source = r->sourcePath;
    QDir().mkpath(m_catalog.stagingRoot());
    const auto tomb = m_catalog.stagingRoot() + "/removed-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto moved = m_dependencies.files->rename(m_catalog.itemPath(id), tomb);
    if (!moved.success)
        return moved;
    m_catalog.forget(id);
    auto result = OperationResult::ok("已删除备份", QFileInfo(source).fileName(), 2000);
    auto removed = m_dependencies.files->remove(tomb);
    if (!removed.success)
        result.warning = "追踪已移除，备份副本未能清理：" + tomb;
    return result;
}
OperationResult BackupEngine::rebuild(const QString &id)
{
    auto checked = require(id, true);
    if (!checked.result.success)
        return checked.result;
    auto before = checked.value, r = before;
    auto git = repository(id);
    auto branch = git.branch();
    if (!branch.result.success)
        return branch.result;
    const auto remoteName = git.remoteName();
    if (!remoteName.result.success)
        return remoteName.result;
    auto remote = git.run({"remote", "get-url", remoteName.value});
    auto lease = remote.success() ? git.remoteHead() : BackupResult<QString>{OperationResult::ok({})};
    QDir().mkpath(m_catalog.stagingRoot());
    QTemporaryDir stage(m_catalog.stagingRoot() + "/rebuild-XXXXXX");
    if (!stage.isValid())
        return OperationResult::fail("重建失败", "无法创建候选仓库");
    const auto candidate = stage.path() + "/repository";
    auto exported = git.exportRevision(r.lastCommit, ".", candidate, *m_dependencies.files);
    if (!exported.success)
        return exported;
    GitRepository fresh(candidate, m_dependencies.git, m_cancel);
    const auto format = git.run({"rev-parse", "--show-object-format"});
    if (!format.success())
        return GitRepository::outcome(format);
    auto initialized = fresh.initialize(branch.value, format.output.trimmed());
    if (!initialized.success)
        return initialized;
    for (const auto *name : {"exclude", "attributes"})
    {
        const auto oldPath = m_catalog.repoPath(id) + "/.git/info/" + name;
        const auto newPath = candidate + "/.git/info/" + name;
        if (!BackupFiles::exists(oldPath))
            continue;
        auto removed = m_dependencies.files->remove(newPath);
        if (!removed.success)
            return removed;
        auto copied = m_dependencies.files->copy(oldPath, newPath);
        if (!copied.success)
            return copied;
    }
    // The candidate contains only the already committed tree. Force here preserves
    // tracked files later matched by .gitignore; it never adds ignored source files.
    auto added = fresh.run({"add", "--all", "--force", "--", "."});
    if (!added.success())
        return GitRepository::outcome(added);
    auto committed = fresh.run({"commit", "--allow-empty", "-m", "Initial backup"});
    if (!committed.success())
        return GitRepository::outcome(committed);
    const auto originalTree = git.run({"rev-parse", r.lastCommit + "^{tree}"});
    const auto rebuiltTree = fresh.run({"rev-parse", "HEAD^{tree}"});
    if (!originalTree.success() || !rebuiltTree.success() || originalTree.output != rebuiltTree.output)
        return OperationResult::fail("重建已取消", "候选仓库未能完整保留已提交内容，原仓库保持不变");
    if (remote.success())
    {
        auto configured = fresh.run({"remote", "add", remoteName.value, remote.output.trimmed()});
        if (!configured.success())
            return GitRepository::outcome(configured);
        auto target = git.targetRef();
        if (!target.result.success)
            return target.result;
        auto merged = fresh.run({"config", "branch." + branch.value + ".merge", target.value});
        if (!merged.success())
            return GitRepository::outcome(merged);
        auto tracked = fresh.run({"config", "branch." + branch.value + ".remote", remoteName.value});
        if (!tracked.success())
            return GitRepository::outcome(tracked);
    }
    BackupReplacement replacement(m_dependencies.files, m_catalog.repoPath(id) + "/.git", m_catalog.itemPath(id));
    if (!replacement.valid())
        return OperationResult::fail("重建失败", "无法暂存旧仓库");
    auto staged = m_dependencies.files->rename(candidate + "/.git", replacement.stagingPath());
    if (!staged.success)
        return staged;
    const auto currentHead = git.head();
    const auto currentBranch = git.branch();
    if (!currentHead.result.success || !currentBranch.result.success || currentHead.value != before.lastCommit || currentBranch.value != branch.value || !git.clean().success)
        return attention(before, "准备重建期间发现外部仓库修改，已停止替换原仓库");
    r.operation = "rebuild";
    r.recoveryPaths = {replacement.recoveryPath()};
    auto saved = m_catalog.save(r);
    if (!saved.success)
        return saved;
    auto installed = replacement.install();
    if (!installed.success)
    {
        auto rolled = replacement.rollback();
        if (!rolled.success)
            return attention(r, rolled.message);
        m_catalog.save(before);
        return installed;
    }
    const auto rebuiltHead = repository(id).head();
    if (!rebuiltHead.result.success)
    {
        replacement.preserve();
        return attention(r, "重建后无法确认提交，旧仓库已保留：" + replacement.recoveryPath());
    }
    r.lastCommit = rebuiltHead.value;
    ++r.generation;
    auto result = completeReplacement(r, replacement, OperationResult::ok("重建完成", "已保留当前版本并清除本地历史"));
    if (!result.success)
        return result;
    if (remote.success())
    {
        auto pushed = lease.result.success ? repository(id).push(true, lease.value) : lease.result;
        if (!pushed.success)
            result.warning = "本地重建已完成，云端覆盖未完成：" + pushed.message;
        else
            result.message += "，云端已同步";
    }
    return result;
}
OperationResult BackupEngine::checkRemote(const QString &url)
{
    if (url.trimmed().isEmpty() || url.trimmed().startsWith('-'))
        return OperationResult::fail("检查失败", "云端仓库地址无效");
    auto result = GitRepository({}, m_dependencies.git, m_cancel).run({"ls-remote", "--heads", "--", url.trimmed()}, {}, true, 300000);
    return result.success() ? OperationResult::ok("检查成功", "仓库地址可访问", 2000) : GitRepository::outcome(result, "检查失败");
}
BackupResult<PreparedImport> BackupEngine::prepareImport(const QString &url)
{
    if (url.trimmed().isEmpty() || url.trimmed().startsWith('-'))
        return {OperationResult::fail("导入失败", "云端仓库地址无效")};
    QDir().mkpath(m_catalog.stagingRoot());
    auto stage = std::make_shared<QTemporaryDir>(m_catalog.stagingRoot() + "/import-XXXXXX");
    if (!stage->isValid())
        return {OperationResult::fail("导入失败", "无法准备临时仓库")};
    const auto path = stage->path() + "/repository";
    const auto cloned = GitRepository({}, m_dependencies.git, m_cancel).run({"clone", "--no-hardlinks", "--", url.trimmed(), path}, {}, true, 300000);
    if (!cloned.success())
        return {GitRepository::outcome(cloned, "导入失败")};
    GitRepository git(path, m_dependencies.git, m_cancel);
    auto contents = git.importEntries();
    if (!contents.result.success)
        return {contents.result};
    for (const auto &args : {QStringList{"config", "user.name", "ZcVersionBox"}, QStringList{"config", "user.email", "backup@zcversionbox.local"}})
    {
        auto result = git.run(args);
        if (!result.success())
            return {GitRepository::outcome(result)};
    }
    PreparedImport prepared{QUuid::createUuid().toString(QUuid::WithoutBraces), contents.value, "."};
    QStringList top;
    for (const auto &entry : contents.value)
        if (entry.path != "." && !entry.path.contains('/'))
            top.append(entry.path);
    if (top.size() == 1)
        prepared.suggestedPath = top.first();
    m_imports.insert(prepared.sessionId, {stage, prepared});
    return {OperationResult::ok({}), prepared};
}
OperationResult BackupEngine::cancelImport(const QString &session)
{
    const auto it = m_imports.find(session);
    if (it == m_imports.end())
        return OperationResult::ok({});
    const auto path = it->directory->path();
    const auto removed = m_dependencies.files->remove(path);
    if (removed.success)
        m_imports.erase(it);
    return removed;
}
OperationResult BackupEngine::finishImport(const QString &session, const QString &entry, const QString &target, bool replaceExisting)
{
    auto found = m_imports.find(session);
    if (found == m_imports.end() || !BackupCatalog::validRepositoryPath(entry))
        return OperationResult::fail("导入失败", "导入会话或仓库内路径无效");
    auto valid = validateSource(target, false);
    if (!valid.success)
        return valid;
    auto source = BackupCatalog::normalizedSource(target);
    if (!m_catalog.idForSource(source).isEmpty())
        return OperationResult::fail("导入失败", "该源位置已有追踪记录");
    auto selected = std::find_if(found->data.entries.cbegin(), found->data.entries.cend(), [&](const ImportEntry &e)
                                 { return e.path == entry; });
    if (selected == found->data.entries.cend())
        return OperationResult::fail("导入失败", "所选内容不存在");
    const auto temporaryRepo = found->directory->path() + "/repository";
    GitRepository git(temporaryRepo, m_dependencies.git, m_cancel);
    const auto clean = git.clean();
    if (!clean.success)
        return clean;
    const bool existing = BackupFiles::exists(source);
    if (existing && !replaceExisting)
        return OperationResult::warn("需要确认覆盖", "目标位置已存在内容");
    SourceFingerprint before;
    if (existing)
    {
        auto scanned = m_dependencies.files->fingerprint(source, QFileInfo(source).isDir(), before, false);
        if (!scanned.success)
            return scanned;
    }
    BackupReplacement replacement(m_dependencies.files, source);
    if (!replacement.valid())
        return OperationResult::fail("导入失败", "无法准备目标位置");
    auto copied = m_dependencies.files->copy(entry == "." ? temporaryRepo : temporaryRepo + '/' + entry, replacement.stagingPath());
    if (!copied.success)
        return copied;
    if (existing != BackupFiles::exists(source))
        return OperationResult::warn("导入已取消", "目标位置发生变化，请重新确认");
    if (existing)
    {
        SourceFingerprint now;
        auto scanned = m_dependencies.files->fingerprint(source, QFileInfo(source).isDir(), now, false);
        if (!scanned.success || now != before)
            return OperationResult::warn("导入已取消", "目标内容发生变化，请重新确认");
    }
    BackupRecord r;
    r.id = newId(source);
    r.sourcePath = source;
    r.repositoryPath = entry;
    r.directory = selected->directory;
    r.generation = m_generations.value(r.id) + 1;
    const auto importedHead = git.head();
    if (!importedHead.result.success)
        return importedHead.result;
    r.lastCommit = importedHead.value;
    if (!BackupCatalog::validId(r.id) || BackupFiles::exists(m_catalog.itemPath(r.id)))
        return OperationResult::fail("导入失败", "追踪标识已存在");
    if (!QDir().mkpath(m_catalog.itemPath(r.id)))
        return OperationResult::fail("导入失败", "无法创建追踪目录");
    auto moved = m_dependencies.files->rename(temporaryRepo, m_catalog.repoPath(r.id));
    if (!moved.success)
    {
        QDir().rmdir(m_catalog.itemPath(r.id));
        return moved;
    }
    // The repository and source candidate are ready. Persist the one unfinished
    // operation marker before the first source rename, so a crash is visible.
    r.operation = "import";
    r.recoveryPaths = {replacement.recoveryPath()};
    auto saved = m_catalog.save(r);
    if (!saved.success)
    {
        const auto movedBack = m_dependencies.files->rename(m_catalog.repoPath(r.id), temporaryRepo);
        if (!movedBack.success)
            saved.warning = "源位置未修改；候选仓库保留在：" + m_catalog.itemPath(r.id);
        QDir().rmdir(m_catalog.itemPath(r.id));
        return saved;
    }
    const auto pendingImport = r;
    const auto abort = [&](OperationResult failure)
    {
        m_dependencies.files->cancellation.reset();
        const auto rolled = replacement.rollback();
        if (!rolled.success)
            return attention(pendingImport, failure.message + "；" + rolled.message);
        const auto movedBack = m_dependencies.files->rename(m_catalog.repoPath(r.id), temporaryRepo);
        if (!movedBack.success)
            return attention(pendingImport, "源位置已恢复；候选仓库无法移回，保留在：" + m_catalog.itemPath(r.id));
        const auto erased = m_catalog.erase(r.id);
        if (!erased.success)
        {
            failure.warning = erased.message;
            return failure;
        }
        QDir().rmdir(m_catalog.itemPath(r.id));
        return failure;
    };
    if (existing != BackupFiles::exists(source))
        return abort(OperationResult::warn("导入已取消", "目标位置发生变化，请重新确认"));
    if (existing)
    {
        SourceFingerprint now;
        auto scanned = m_dependencies.files->fingerprint(source, QFileInfo(source).isDir(), now, false);
        if (!scanned.success || now != before)
            return abort(OperationResult::warn("导入已取消", "目标内容发生变化，请重新确认"));
    }
    const auto installed = replacement.install();
    if (!installed.success)
        return abort(installed);
    SourceFingerprint imported;
    auto scanned = m_dependencies.files->fingerprint(source, r.directory, imported);
    if (!scanned.success)
        return abort(scanned);
    auto intact = replacement.verifyInstalled();
    if (!intact.success)
    {
        replacement.preserve();
        return attention(r, intact.message);
    }
    r.fingerprint = std::move(imported);
    r.operation.clear();
    r.recoveryPaths.clear();
    saved = m_catalog.save(r);
    if (!saved.success)
        return abort(saved);
    auto result = OperationResult::ok("导入成功", "已保留原仓库历史并加入追踪");
    m_dependencies.files->cancellation.reset();
    auto cleaned = replacement.finish();
    if (!cleaned.success)
        result.warning = cleaned.message;
    m_generations[r.id] = r.generation;
    m_imports.remove(session);
    return result;
}
OperationResult BackupEngine::recheck(const QString &id)
{
    auto checked = require(id);
    if (!checked.result.success)
        return checked.result;
    auto r = checked.value;
    if (r.state == BackupSyncState::RemotePending)
        return paused(r);
    auto git = repository(id);
    auto clean = git.clean();
    if (!clean.success)
        return clean;
    for (const auto &path : r.recoveryPaths)
        if (BackupFiles::exists(path))
            return OperationResult::warn("需要检查", "请先核对保留副本并处理未完成操作：" + path);
    auto current = git.head();
    if (!current.result.success)
        return current.result;
    r.operation.clear();
    if (current.value != r.lastCommit)
    {
        r.state = BackupSyncState::RemotePending;
        r.pendingCommit = current.value;
        r.stateDetail = "仓库版本已改变，请确认如何处理源位置";
    }
    else
    {
        r.state = BackupSyncState::Tracking;
        r.stateDetail.clear();
    }
    return m_catalog.save(r);
}
