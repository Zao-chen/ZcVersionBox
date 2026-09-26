#include "backup_git.h"
#include "backup_catalog.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimeZone>

GitRepository::GitRepository(QString path, GitRunner runner, std::shared_ptr<std::atomic_bool> cancel)
    : m_path(std::move(path)), m_runner(std::move(runner)), m_cancel(std::move(cancel)) {}
GitResult GitRepository::run(const QStringList &args, const QByteArray &input, bool cancellable, int timeoutMs) const
{
    // No source hooks, signing prompts, pager, or optional index refresh in read commands.
    QStringList command{"--no-pager", "--literal-pathspecs", "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false", "-c", "core.quotePath=false"};
#ifdef Q_OS_WIN
    command << "-c" << "core.longpaths=true";
#endif
    command += args;
    return m_runner(m_path, command, {timeoutMs, input, cancellable ? m_cancel : nullptr});
}
OperationResult GitRepository::outcome(const GitResult &r, const QString &title)
{
    if (r.cancelled)
        return OperationResult::cancel("操作已取消", r.error);
    return r.success() ? OperationResult::ok({}) : OperationResult::fail(title, r.error);
}
OperationResult GitRepository::validate() const
{
    if (BackupFiles::hasLinkedAncestor(m_path + "/.git") || !QFileInfo(m_path + "/.git").isDir())
        return OperationResult::fail("仓库不可用", "同步目录缺少独立的 Git 仓库，或路径已被链接替换");
    for (const auto *part : {"objects", "refs", "logs", "index", "config"})
        if (BackupFiles::hasLinkedAncestor(m_path + "/.git/" + part))
            return OperationResult::fail("仓库不可用", "Git 存储包含外部链接，请先检查仓库");
    auto root = run({"rev-parse", "--show-toplevel"});
    if (!root.success() || QFileInfo(root.output.trimmed()) != QFileInfo(m_path))
        return OperationResult::fail("仓库不可用", "Git 工作目录与追踪记录不匹配");
    return OperationResult::ok({});
}
OperationResult GitRepository::initialize(const QString &branchName, const QString &objectFormat) const
{
    if (BackupFiles::hasLinkedAncestor(m_path) || BackupFiles::exists(m_path + "/.git"))
        return OperationResult::fail("初始化已取消", "候选目录必须不含现有 Git 元数据，不能复用 Git 指针或外部仓库");
    auto args = QStringList{"init"};
    if (!branchName.isEmpty())
        args << "--initial-branch=" + branchName;
    if (!objectFormat.isEmpty())
        args << "--object-format=" + objectFormat;
    auto result = run(args);
    if (!result.success())
        return outcome(result);
    const auto checked = validate();
    if (!checked.success)
        return checked;
    for (const auto &config : {QStringList{"config", "user.name", "ZcVersionBox"}, QStringList{"config", "user.email", "backup@zcversionbox.local"}})
    {
        result = run(config);
        if (!result.success())
            return outcome(result);
    }
    return OperationResult::ok({});
}
BackupResult<QString> GitRepository::head() const { return resolve("HEAD"); }
BackupResult<QString> GitRepository::resolve(const QString &revision) const
{
    const auto r = run({"rev-parse", "--verify", "--end-of-options", revision + "^{commit}"});
    return {outcome(r), r.output.trimmed()};
}
BackupResult<QString> GitRepository::branch() const
{
    auto r = run({"symbolic-ref", "--quiet", "--short", "HEAD"});
    if (!r.success())
        return {OperationResult::fail("操作已暂停", "同步仓库未处于工作分支，请先完成外部 Git 操作")};
    return {OperationResult::ok({}), r.output.trimmed()};
}
OperationResult GitRepository::clean() const
{
    for (const auto *name : {"index.lock", "MERGE_HEAD", "CHERRY_PICK_HEAD", "REVERT_HEAD", "rebase-merge", "rebase-apply"})
        if (BackupFiles::exists(m_path + "/.git/" + name))
            return OperationResult::fail("操作已暂停", "同步仓库有 Git 锁或未完成操作，请先检查仓库");
    const auto result = run({"--no-optional-locks", "status", "--porcelain=v1", "-z", "--untracked-files=normal"});
    if (!result.success())
        return outcome(result);
    return result.output.isEmpty() ? OperationResult::ok({}) : OperationResult::fail("操作已暂停", "同步仓库有未提交的外部修改，已保留内容，请先处理仓库");
}
BackupResult<QString> GitRepository::stagedState(const QString &managedPath) const
{
    if (!BackupCatalog::validRepositoryPath(managedPath))
        return {OperationResult::fail("暂存区检查失败", "受管理路径无效")};
    // Disable rename detection so every changed path (including deletions) is
    // checked independently. NUL separators preserve spaces, tabs and newlines.
    const auto result = run({"diff", "--cached", "--raw", "--no-renames", "--no-abbrev", "-z"});
    if (!result.success())
        return {outcome(result)};
    const auto fields = result.output.split(QChar(0), Qt::SkipEmptyParts);
    if (fields.size() % 2 != 0)
        return {OperationResult::fail("暂存区检查失败", "无法解析完整暂存状态")};
    for (qsizetype i = 0; i < fields.size(); i += 2)
    {
        const auto &path = fields[i + 1];
        if (!fields[i].startsWith(':') || !BackupCatalog::validRepositoryPath(path))
            return {OperationResult::fail("暂存区检查失败", "暂存区包含无法确认的路径")};
        if (managedPath != "." && path != managedPath && !path.startsWith(managedPath + '/'))
            return {OperationResult::fail("操作已暂停", "发现本次备份范围以外的暂存修改，已保留现场：" + path)};
    }
    return {OperationResult::ok({}), result.output};
}
BackupResult<QVector<Revision>> GitRepository::history() const
{
    const auto r = run({"log", "-z", "--format=%H%x00%h%x00%ct%x00%s"});
    if (!r.success())
        return {outcome(r, "打开备份失败")};
    QVector<Revision> revisions;
    const auto fields = r.output.split(QChar(0), Qt::KeepEmptyParts);
    for (qsizetype i = 0; i + 3 < fields.size(); i += 4)
    {
        bool valid = false;
        const auto time = fields[i + 2].toLongLong(&valid);
        if (!valid || fields[i].isEmpty())
            return {OperationResult::fail("打开备份失败", "无法读取提交记录")};
        revisions.append({fields[i], fields[i + 3], QDateTime::fromSecsSinceEpoch(time, QTimeZone::UTC), fields[i + 1]});
    }
    return {OperationResult::ok({}), revisions};
}
BackupResult<DiffData> GitRepository::diff(const QString &revision) const
{
    const auto resolved = resolve(revision);
    if (!resolved.result.success)
        return {resolved.result};
    DiffData data;
    data.newCommit = resolved.value;
    auto parents = run({"rev-list", "--parents", "-n", "1", data.newCommit});
    if (!parents.success())
        return {outcome(parents)};
    auto commits = parents.output.trimmed().split(' ', Qt::SkipEmptyParts);
    if (commits.size() > 1)
        data.oldCommit = commits[1];
    const auto arguments = [&](const QString &format)
    {
        if (data.oldCommit.isEmpty())
            return QStringList{"diff-tree", "--root", "--no-commit-id", "-r", "--find-renames", format, "-z", data.newCommit};
        return QStringList{"diff", "--find-renames", format, "-z", data.oldCommit, data.newCommit};
    };
    auto statsResult = run(arguments("--numstat"));
    if (!statsResult.success())
        return {outcome(statsResult)};
    QMap<QString, QString> stats;
    const auto records = statsResult.output.split(QChar(0), Qt::KeepEmptyParts);
    for (qsizetype i = 0; i < records.size(); ++i)
    {
        auto a = records[i].indexOf('\t'), b = records[i].indexOf('\t', a + 1);
        if (a < 0 || b < 0)
            continue;
        const auto added = records[i].left(a), removed = records[i].mid(a + 1, b - a - 1);
        auto path = records[i].mid(b + 1);
        if (path.isEmpty() && i + 2 < records.size())
        {
            i += 2;
            path = records[i];
        }
        stats[path] = added == "-" || removed == "-" ? QStringLiteral("二进制") : QString("+%1 / -%2").arg(added, removed);
    }
    auto names = run(arguments("--name-status"));
    if (!names.success())
        return {outcome(names)};
    const auto paths = names.output.split(QChar(0), Qt::KeepEmptyParts);
    for (qsizetype i = 0; i + 1 < paths.size();)
    {
        auto status = paths[i++], path = paths[i++];
        if ((status.startsWith('R') || status.startsWith('C')) && i < paths.size())
            path = paths[i++];
        if (!status.isEmpty() && !path.isEmpty())
            data.files.append({status, path, stats.value(path, "-")});
    }
    return {OperationResult::ok({}), data};
}
BackupResult<QString> GitRepository::diffText(const DiffData &data, const QString &file) const
{
    if (!resolve(data.newCommit).result.success || (!data.oldCommit.isEmpty() && !resolve(data.oldCommit).result.success))
        return {OperationResult::fail("加载对比失败", "提交上下文已失效")};
    QStringList args = data.oldCommit.isEmpty() ? QStringList{"diff-tree", "--root", "--no-commit-id", "-r", "-p"} : QStringList{"diff"};
    args << "--no-color" << "--no-ext-diff" << "--no-textconv" << (file.isEmpty() ? "--unified=20" : "--unified=80");
    if (!data.oldCommit.isEmpty())
        args << data.oldCommit;
    args << data.newCommit << "--";
    if (!file.isEmpty())
        args << file;
    const auto r = run(args);
    return {outcome(r, "加载对比失败"), r.output};
}
OperationResult GitRepository::exportRevision(const QString &revision, const QString &relativePath, const QString &target, const BackupFiles &files) const
{
    if (!BackupCatalog::validRepositoryPath(relativePath))
        return OperationResult::fail("导出失败", "仓库内路径无效");
    const auto oid = resolve(revision);
    if (!oid.result.success)
        return oid.result;
    const auto mapping = validateMapping(oid.value, relativePath);
    if (!mapping.success)
        return mapping;
    QTemporaryDir temp(QDir(QDir::tempPath()).canonicalPath() + "/ZcVersionBoxSnapshot-XXXXXX");
    if (!temp.isValid())
        return OperationResult::fail("导出失败", "无法创建临时目录");
    const auto worktree = temp.path() + "/tree";
    auto added = run({"worktree", "add", "--detach", worktree, oid.value});
    OperationResult result;
    if (!added.success())
        result = outcome(added, "读取版本失败");
    else
    {
        const auto source = relativePath == "." ? worktree : QDir(worktree).filePath(relativePath);
        result = BackupFiles::exists(source) ? files.copy(source, target) : OperationResult::fail("读取版本失败", "所选版本中不存在追踪内容，源位置未修改");
    }
    const auto removed = run({"worktree", "remove", "--force", worktree}, {}, false);
    if (!removed.success() && QFileInfo::exists(worktree))
    {
        temp.setAutoRemove(false);
        result.warning = "临时版本目录未能清理：" + temp.path();
    }
    return result;
}
BackupResult<QVector<ImportEntry>> GitRepository::importEntries() const
{
    auto result = run({"ls-tree", "-r", "-t", "-z", "--full-tree", "HEAD"});
    if (!result.success())
        return {outcome(result, "导入失败")};
    QVector<ImportEntry> entries{{".", true}};
    for (const auto &record : result.output.split(QChar(0), Qt::SkipEmptyParts))
    {
        const auto tab = record.indexOf('\t');
        if (tab < 0)
            return {OperationResult::fail("导入失败", "无法读取 Git 文件树")};
        const auto path = record.mid(tab + 1);
        if (!BackupCatalog::validRepositoryPath(path))
            return {OperationResult::fail("导入失败", "仓库包含不安全的文件路径")};
        if (record.startsWith("160000") || record.startsWith("120000"))
            return {OperationResult::fail("导入失败", "仓库包含暂不支持的子模块或符号链接：" + path)};
        entries.append({path, record.startsWith("040000")});
    }
    return {OperationResult::ok({}), entries};
}
BackupResult<QString> GitRepository::targetRef() const
{
    const auto current = branch();
    if (!current.result.success)
        return current;
    auto upstream = run({"config", "--get", "branch." + current.value + ".merge"});
    return {OperationResult::ok({}), upstream.success() && upstream.output.trimmed().startsWith("refs/heads/") ? upstream.output.trimmed() : "refs/heads/" + current.value};
}
BackupResult<QString> GitRepository::remoteName() const
{
    const auto current = branch();
    if (!current.result.success)
        return current;
    const auto configured = run({"config", "--get", "branch." + current.value + ".remote"});
    if (!configured.success() && configured.exitCode != 1)
        return {outcome(configured)};
    const auto name = configured.success() ? configured.output.trimmed() : QStringLiteral("origin");
    if (name.isEmpty() || name == "." || name.startsWith('-') || name.contains('\n'))
        return {OperationResult::fail("远程配置无效", "当前分支的 upstream 不是可用的远程仓库")};
    return {OperationResult::ok({}), name};
}
OperationResult GitRepository::validateMapping(const QString &commit, const QString &path, std::optional<bool> directory) const
{
    if (!BackupCatalog::validRepositoryPath(path))
        return OperationResult::fail("映射无效", "仓库内路径无效");
    const auto oid = resolve(commit);
    if (!oid.result.success)
        return oid.result;
    const auto type = run({"cat-file", "-t", path == "." ? oid.value + "^{tree}" : oid.value + ':' + path});
    const auto kind = type.output.trimmed();
    if (!type.success() || (kind != "tree" && kind != "blob") || (directory && kind != (*directory ? "tree" : "blob")))
        return OperationResult::fail("追踪内容不可用", "该版本已删除追踪路径或改变其类型，源位置保持原样。请检查仓库内路径：" + path);
    const auto entries = run({"ls-tree", "-r", "-z", oid.value, "--", path});
    if (!entries.success())
        return outcome(entries);
    for (const auto &entry : entries.output.split(QChar(0), Qt::SkipEmptyParts))
        if (entry.startsWith("120000") || entry.startsWith("160000"))
            return OperationResult::fail("追踪内容不可用", "所选版本包含暂不支持的符号链接或子模块：" + entry.section('\t', 1));
    return OperationResult::ok({});
}
OperationResult GitRepository::push(bool forceWithLease, const QString &expectedRemote) const
{
    const auto target = targetRef();
    if (!target.result.success)
        return target.result;
    const auto remote = remoteName();
    if (!remote.result.success)
        return remote.result;
    QStringList args{"push", "--set-upstream"};
    if (forceWithLease)
        args << "--force-with-lease=" + target.value + ":" + expectedRemote;
    args << remote.value << "HEAD:" + target.value;
    return outcome(run(args, {}, true, 300000), "上传失败");
}
OperationResult GitRepository::pull() const
{
    const auto target = targetRef();
    if (!target.result.success)
        return target.result;
    const auto remote = remoteName();
    if (!remote.result.success)
        return remote.result;
    auto result = run({"fetch", "--no-tags", remote.value, target.value}, {}, true, 300000);
    if (!result.success())
        return outcome(result, "拉取失败");
    return outcome(run({"merge", "--ff-only", "--no-edit", "FETCH_HEAD"}), "无法快进拉取");
}
BackupResult<QString> GitRepository::remoteHead() const
{
    const auto target = targetRef();
    if (!target.result.success)
        return target;
    const auto remote = remoteName();
    if (!remote.result.success)
        return remote;
    const auto result = run({"ls-remote", remote.value, target.value}, {}, true, 300000);
    return {outcome(result), result.output.section('\t', 0, 0).trimmed()};
}
