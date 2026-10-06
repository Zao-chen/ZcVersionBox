#include "backup_git.h"
#include <QCryptographicHash>
#include <QSet>
#include <QTimeZone>

BackupResult<QString> GitRepository::branchRef() const
{
    const auto result = run({"symbolic-ref", "--quiet", "HEAD"});
    if (!result.success() || !result.output.trimmed().startsWith("refs/heads/"))
        return {OperationResult::fail("操作已暂停", "仓库未处于本地方案，请先完成外部 Git 操作")};
    return {OperationResult::ok({}), result.output.trimmed()};
}

OperationResult GitRepository::validateBranchName(const QString &name) const
{
    if (name.isEmpty() || name.startsWith('-') || name == "HEAD" || name != name.trimmed() ||
        name.contains(QChar(0)) || !run({"check-ref-format", "refs/heads/" + name}).success())
        return OperationResult::fail("方案名称无效", "名称可以使用中文；不能包含空格、..、~、^、:、?、*、[、反斜线，也不能以横线开头或以 .lock 结尾。");
    return OperationResult::ok({});
}

BackupResult<QVector<BranchInfo>> GitRepository::branches() const
{
    const auto result = run({"for-each-ref", "--sort=refname",
        "--format=%(refname)%00%(objectname)%00%(upstream)%00%(symref)%00%(subject)%00%(upstream:remotename)%00%(upstream:remoteref)",
        "refs/heads/", "refs/remotes/"});
    if (!result.success()) return {outcome(result)};
    const auto current = branchRef();
    const auto remotes = run({"remote"});
    if (!remotes.success()) return {outcome(remotes)};
    auto remoteNames = remotes.output.split('\n', Qt::SkipEmptyParts);
    std::sort(remoteNames.begin(), remoteNames.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    QVector<BranchInfo> values;
    for (auto line : result.output.split('\n', Qt::SkipEmptyParts))
    {
        const auto fields = line.split(QChar(0));
        if (fields.size() != 7) return {OperationResult::fail("读取方案失败", "无法解析分支引用")};
        if (!fields[3].isEmpty()) continue; // remote HEAD is an alias, not a branch.
        BranchInfo value;
        value.ref = fields[0]; value.head = fields[1]; value.upstream = fields[2]; value.message = fields[4];
        value.remote = fields[5]; value.remoteRef = fields[6];
        value.remoteBranch = value.ref.startsWith("refs/remotes/");
        value.current = current.result.success && current.value == value.ref;
        value.name = value.ref.mid(value.remoteBranch ? 13 : 11);
        if (value.remoteBranch)
            for (const auto &remote : remoteNames)
                if (value.name.startsWith(remote + '/'))
                { value.remote = remote; value.remoteRef = "refs/heads/" + value.name.mid(remote.size() + 1); break; }
        values.append(value);
    }
    return {OperationResult::ok({}), values};
}

BackupResult<HistoryPage> GitRepository::branchHistory(const HistoryQuery &query) const
{
    if (query.offset < 0 || query.limit < 1 || query.limit > 500)
        return {OperationResult::fail("读取历史失败", "分页参数无效")};
    HistoryPage page;
    const auto refs = branches();
    if (!refs.result.success) return {refs.result};
    QStringList tips = query.tips;
    if (tips.isEmpty())
    {
        if (query.allBranches) for (const auto &branch : refs.value) tips.append(branch.head);
        else tips.append("HEAD");
    }
    for (const auto &tip : tips)
    {
        const auto resolved = resolve(tip);
        if (!resolved.result.success) return {resolved.result};
        if (!page.tips.contains(resolved.value)) page.tips.append(resolved.value);
    }
    if (page.tips.isEmpty()) return {OperationResult::ok({}), page};
    QStringList args{"log", "--topo-order", "-z", "--format=%H%x00%h%x00%ct%x00%s%x00%P",
                     "--skip=" + QString::number(query.offset), "--max-count=" + QString::number(query.limit + 1)};
    args += page.tips; args << "--";
    const auto log = run(args);
    if (!log.success()) return {outcome(log)};
    const auto tagsResult = tags();
    if (!tagsResult.result.success) return {tagsResult.result};
    const auto fields = log.output.split(QChar(0));
    for (qsizetype i = 0; i + 4 < fields.size(); i += 5)
    {
        bool valid = false;
        const auto time = fields[i + 2].toLongLong(&valid);
        if (!valid) return {OperationResult::fail("读取历史失败", "提交时间无效")};
        Revision revision{fields[i], fields[i + 3], QDateTime::fromSecsSinceEpoch(time, QTimeZone::UTC), fields[i + 1], {}};
        revision.parents = fields[i + 4].split(' ', Qt::SkipEmptyParts);
        for (const auto &ref : refs.value) if (ref.head == revision.hash) revision.refs.append(ref.name);
        for (const auto &tag : tagsResult.value) if (tag.commitOid == revision.hash) revision.tags.append(tag);
        page.revisions.append(revision);
    }
    page.hasMore = page.revisions.size() > query.limit;
    if (page.hasMore) page.revisions.resize(query.limit);
    return {OperationResult::ok({}), page};
}

BackupResult<QString> GitRepository::branchEndpoint() const
{
    const auto endpoint = tagEndpoint(), ref = targetRef();
    if (!endpoint.result.success) return endpoint;
    if (!ref.result.success) return ref;
    if (endpoint.value.isEmpty()) return {OperationResult::ok({}), {}};
    return {OperationResult::ok({}), QString::fromLatin1(QCryptographicHash::hash(
        endpoint.value.toUtf8() + '\0' + ref.value.toUtf8(), QCryptographicHash::Sha256).toHex())};
}
