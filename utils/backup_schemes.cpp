#include "backup_schemes.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>

namespace
{
QString identity() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString ref(const QString &name) { return "refs/heads/" + name; }
OperationResult invalid(const QString &detail)
{ return OperationResult::warn("方案目录需要检查", detail); }
QByteArray encode(const QMap<QString, SharedScheme> &schemes)
{
    QJsonObject entries;
    for (auto it = schemes.cbegin(); it != schemes.cend(); ++it)
        entries.insert(it.key(), QJsonObject{{"name", it->name}, {"head", it->head}, {"deleted", it->deleted}});
    return QJsonDocument(QJsonObject{{"format", 1}, {"schemes", entries}}).toJson(QJsonDocument::Compact);
}
OperationResult associate(const GitRepository &git, const QString &name, const QString &remote, const QString &target)
{
    auto result = GitRepository::outcome(git.run({"config", "branch." + name + ".remote", remote}));
    return result.success ? GitRepository::outcome(git.run({"config", "branch." + name + ".merge", ref(target)})) : result;
}
void recordPublication(BackupRecord &record, const QMap<QString, SharedScheme> &directory, const QString &endpoint, const QString &revision)
{
    record.schemes = directory; record.schemeBase = directory; record.schemeRevision = revision;
    for (const auto &scheme : directory)
    {
        if (scheme.deleted) continue;
        const auto key = QString::fromLatin1(QCryptographicHash::hash(endpoint.toUtf8() + '\0' + ref(scheme.name).toUtf8(), QCryptographicHash::Sha256).toHex());
        auto &baseline = record.branchRemotes[key];
        baseline.lastUploadedHead = scheme.head;
        baseline.rebuildHead.clear(); baseline.rebuildExpected.clear();
    }
}

}

OperationResult BackupSchemes::track(BackupRecord &record, const GitRepository &git)
{
    const auto branches = git.branches();
    if (!branches.result.success) return branches.result;
    const auto remote = git.remoteName();
    if (!remote.result.success) return remote.result;
    if (git.run({"remote", "get-url", remote.value}).success())
    {
        const auto endpoint = git.remoteEndpoint(remote.value);
        if (!endpoint.result.success) return endpoint.result;
        if (!record.schemeEndpoint.isEmpty() && record.schemeEndpoint != endpoint.value)
        { record.schemes.clear(); record.schemeBase.clear(); record.schemeRevision.clear(); }
        record.schemeEndpoint = endpoint.value;
    }
    for (const auto &branch : branches.value)
    {
        if (branch.remoteBranch) continue;
        QString id;
        for (auto it = record.schemes.cbegin(); it != record.schemes.cend(); ++it)
            if (!it->deleted && it->name == branch.name) { id = it.key(); break; }
        if (id.isEmpty()) { id = identity(); record.schemes[id] = {branch.name, branch.head, false}; }
        // Before the first directory exchange, remember an existing upstream as
        // the identity's old name. Renaming offline must not lose this mapping.
        if (!record.schemeBase.contains(id) && !branch.upstream.isEmpty())
            record.schemeBase[id] = {branch.remoteRef.mid(11), git.resolve(branch.upstream).value, false};
    }
    return OperationResult::ok({});
}

BackupResult<SchemePlan> BackupSchemes::read(const BackupRecord &record, const GitRepository &git)
{
    SchemePlan plan;
    const auto remote = git.remoteName();
    if (!remote.result.success) return {remote.result};
    plan.remote = remote.value;
    const auto endpoint = git.remoteEndpoint(plan.remote);
    if (!endpoint.result.success) return {endpoint.result};
    plan.endpoint = endpoint.value;
    const auto fetch = git.run({"remote", "get-url", "--all", plan.remote});
    const auto push = git.run({"remote", "get-url", "--push", "--all", plan.remote});
    if (!fetch.success() || !push.success()) return {GitRepository::outcome(!fetch.success() ? fetch : push)};
    if (fetch.output.trimmed() != push.output.trimmed() || fetch.output.trimmed().contains('\n'))
        return {invalid("自动同步需要一个读取与写入一致的共享仓库地址，请统一仓库配置。")};
    const auto refs = git.branches();
    if (!refs.result.success) return {refs.result};
    for (const auto &branch : refs.value)
    {
        if (branch.remoteBranch && branch.remote == plan.remote) plan.remoteHeads[branch.remoteRef.mid(11)] = branch.head;
        if (!branch.remoteBranch && !branch.remote.isEmpty() && branch.remote != plan.remote)
            return {invalid("方案“" + branch.name + "”关联了其他仓库，请统一共享仓库配置。")};
    }
    const auto advertised = git.run({"ls-remote", "--refs", "--", plan.remote, DirectoryRef}, {}, true, 300000);
    if (!advertised.success()) return {GitRepository::outcome(advertised, "读取共享方案失败")};
    if (!advertised.output.trimmed().isEmpty())
    {
        const auto fetched = git.run({"fetch", "--prune", "--no-tags", "--no-write-fetch-head", "--", plan.remote,
            QString("+") + DirectoryRef + ":refs/zcversionbox/schemes-received",
            "+refs/heads/*:refs/remotes/" + plan.remote + "/*"}, {}, true, 300000);
        if (!fetched.success()) return {GitRepository::outcome(fetched, "获取共享方案失败")};
        const auto revision = git.resolve("refs/zcversionbox/schemes-received");
        if (!revision.result.success) return {revision.result};
        plan.revision = revision.value;
        if (record.schemeEndpoint == plan.endpoint && !record.schemeRevision.isEmpty() &&
            record.schemeRevision != plan.revision)
        {
            const auto lineage = git.run({"merge-base", "--is-ancestor", record.schemeRevision, plan.revision});
            if (!lineage.success()) return {invalid("共享方案目录被改写，已保留本机工作。请恢复方案目录的连续历史后重试。")};
        }
        const auto data = git.run({"show", plan.revision + ":schemes.json"});
        if (!data.success()) return {GitRepository::outcome(data, "读取方案目录失败")};
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(data.bytes, &error).object();
        if (error.error != QJsonParseError::NoError || json["format"].toInt() != 1 || !json["schemes"].isObject())
            return {invalid("共享方案目录格式无效，已停止同步以保留现有版本。")};
        const auto entries = json["schemes"].toObject();
        QSet<QString> names;
        static const QRegularExpression oid("^(?:[0-9a-f]{40}|[0-9a-f]{64})$");
        for (auto it = entries.begin(); it != entries.end(); ++it)
        {
            const auto entry = it.value().toObject();
            SharedScheme scheme{entry["name"].toString(), entry["head"].toString(), entry["deleted"].toBool()};
            if (QUuid(it.key()).isNull() || QUuid(it.key()).toString(QUuid::WithoutBraces) != it.key() ||
                !entry["deleted"].isBool() || !git.validateBranchName(scheme.name).success ||
                !oid.match(scheme.head).hasMatch() || (!scheme.deleted && names.contains(scheme.name)))
                return {invalid("共享方案身份、名称或版本无效，尚未改动本机方案。")};
            if (!scheme.deleted) names.insert(scheme.name);
            if (scheme.deleted && git.resolve(scheme.head + "^{commit}").value != scheme.head)
                return {invalid("已删除方案的保留版本无法读取，尚未改动本机方案。")};
            plan.directory.insert(it.key(), scheme);
        }
    }
    else if (record.schemeEndpoint == plan.endpoint && !record.schemeRevision.isEmpty())
        return {invalid("共享方案目录已被移除。请恢复目录后重试，避免重新上传已删除的方案。")};

    const auto consistent = git.branches();
    if (!consistent.result.success) return {consistent.result};
    plan.remoteHeads.clear();
    for (const auto &branch : consistent.value)
        if (branch.remoteBranch && branch.remote == plan.remote) plan.remoteHeads[branch.remoteRef.mid(11)] = branch.head;
    // Native Git users can add or remove branches too. Deleted identities are
    // retained, so old clients can never turn a missing reference into a create.
    QSet<QString> known;
    for (auto it = plan.directory.begin(); it != plan.directory.end(); ++it)
    {
        if (it->deleted) continue;
        known.insert(it->name);
        if (plan.remoteHeads.contains(it->name)) it->head = plan.remoteHeads.value(it->name);
        else it->deleted = true;
    }
    for (auto it = plan.remoteHeads.cbegin(); it != plan.remoteHeads.cend(); ++it)
    {
        if (known.contains(it.key())) continue;
        bool retired = false;
        for (const auto &entry : plan.directory) if (entry.deleted && entry.name == it.key()) retired = true;
        if (retired) return {invalid("已删除的方案“" + it.key() + "”被旧客户端重新上传。请将其中需要保留的工作改为新名称，再移除旧名称并重试。")};
        plan.directory[identity()] = {it.key(), it.value(), false};
    }
    return {OperationResult::ok({}), plan};
}

OperationResult BackupSchemes::reconcile(BackupRecord &record, const GitRepository &git, SchemePlan &plan, bool &writesStarted)
{
    writesStarted = false;
    const auto originalBranch = record.branchRef;
    auto result = track(record, git);
    if (!result.success) return result;
    const auto branches = git.branches();
    if (!branches.result.success) return branches.result;
    QMap<QString, QString> localHeads;
    for (const auto &branch : branches.value) if (!branch.remoteBranch) localHeads[branch.name] = branch.head;

    // Adopt identities when upgrading a pre-directory repository. Existing
    // renamed upstreams are matched before local names.
    const auto originalIds = record.schemes.keys();
    for (const auto &id : originalIds)
    {
        if (plan.directory.contains(id)) continue;
        const auto scheme = record.schemes.value(id);
        const auto base = record.schemeBase.value(id);
        const auto name = base.name.isEmpty() ? scheme.name : base.name;
        for (auto it = plan.directory.cbegin(); it != plan.directory.cend(); ++it)
        {
            if (it->name != name || (it->deleted && base.name.isEmpty())) continue;
            if (record.schemes.contains(it.key()))
            {
                if (record.schemes.value(it.key()).deleted) continue;
                return invalid("多个方案对应同一共享身份，请先处理重复名称。");
            }
            record.schemes[it.key()] = scheme;
            record.schemes.remove(id);
            if (record.schemeBase.contains(id)) record.schemeBase[it.key()] = record.schemeBase.take(id);
            break;
        }
    }
    QMap<QString, SharedScheme> desired = record.schemes;
    QMap<QString, QString> oldNames;
    for (auto it = desired.cbegin(); it != desired.cend(); ++it)
        if (!it->deleted && localHeads.contains(it->name)) oldNames[it.key()] = it->name;
    for (auto it = plan.directory.cbegin(); it != plan.directory.cend(); ++it)
    {
        const auto id = it.key();
        const auto remote = it.value();
        if (!desired.contains(id)) { desired[id] = remote; continue; }
        auto &ours = desired[id];
        const auto base = record.schemeBase.value(id);
        const auto localHead = localHeads.value(ours.name);
        if (remote.deleted)
        {
            if (!ours.deleted && !localHead.isEmpty())
            {
                const auto ancestor = git.run({"merge-base", "--is-ancestor", localHead, remote.head});
                if (!ancestor.success() && (!ancestor.finished || ancestor.exitCode != 1)) return GitRepository::outcome(ancestor);
                if (ref(ours.name) == record.branchRef || !ancestor.success())
                {
                    const auto keptId = identity();
                    desired[keptId] = {ours.name + "-保留-" + keptId.left(8), localHead, false};
                    oldNames[keptId] = oldNames.take(id);
                    plan.warnings.append("方案“" + remote.name + "”已删除；正在使用或尚未同步的工作保留为“" + desired[keptId].name + "”。");
                }
            }
            ours = remote;
            continue;
        }
        if (ours.deleted)
        {
            const auto covered = git.run({"merge-base", "--is-ancestor", remote.head, ours.head});
            if (!covered.success() && (!covered.finished || covered.exitCode != 1)) return GitRepository::outcome(covered);
            if (!covered.success() || (!base.name.isEmpty() && remote.name != base.name))
            {
                ours = remote;
                plan.warnings.append("未删除“" + remote.name + "”：协作者已更新该方案，请查看后重新确认删除。");
            }
            continue;
        }
        const bool renamed = !base.name.isEmpty() && ours.name != base.name;
        if (!renamed || (remote.name != base.name && remote.name != ours.name))
        {
            if (renamed) plan.warnings.append("方案已被协作者改名，采用共享名称“" + remote.name + "”，版本内容均已保留。");
            ours.name = remote.name;
        }
        ours.head = localHead.isEmpty() ? remote.head : localHead;
    }
    QSet<QString> names;
    // Reserve accepted names first; a concurrent create/rename keeps its work
    // under a unique name instead of replacing the other identity.
    auto ids = desired.keys();
    std::stable_sort(ids.begin(), ids.end(), [&](const QString &a, const QString &b) {
        const auto accepted = [&](const QString &id) { return plan.directory.contains(id) && !desired[id].deleted && desired[id].name == plan.directory[id].name; };
        return accepted(a) && !accepted(b);
    });
    for (const auto &id : ids)
    {
        auto &scheme = desired[id];
        if (scheme.deleted) continue;
        if (names.contains(scheme.name))
        {
            const auto old = scheme.name;
            do { scheme.name = old + "-保留-" + identity().left(8); } while (names.contains(scheme.name) || localHeads.contains(scheme.name));
            plan.warnings.append("名称“" + old + "”已被使用，另一份工作保留为“" + scheme.name + "”。");
        }
        names.insert(scheme.name);
    }
    const auto worktrees = git.run({"worktree", "list", "--porcelain"});
    if (!worktrees.success()) return GitRepository::outcome(worktrees);
    QMap<QString, QString> temporary;
    for (auto it = oldNames.cbegin(); it != oldNames.cend(); ++it)
    {
        const auto target = desired.value(it.key());
        if (!target.deleted && it.value() == target.name) continue;
        if (ref(it.value()) != record.branchRef && worktrees.output.split('\n').contains("branch " + ref(it.value())))
            return invalid("方案“" + it.value() + "”正在其他 Git 工作目录使用，请先关闭该工作目录再同步。");
    }
    writesStarted = true;
    for (auto it = oldNames.cbegin(); it != oldNames.cend(); ++it)
    {
        const auto target = desired.value(it.key());
        if (target.deleted)
        {
            result = GitRepository::outcome(git.run({"update-ref", "-d", ref(it.value()), localHeads.value(it.value())}));
            if (!result.success) return result;
            const auto cleaned = git.run({"config", "--remove-section", "branch." + it.value()});
            if (!cleaned.success() && cleaned.exitCode != 5) return GitRepository::outcome(cleaned);
        }
        else if (it.value() != target.name)
        {
            const auto staging = "zc-scheme-" + identity();
            result = GitRepository::outcome(git.run({"branch", "-m", "--", it.value(), staging}));
            if (!result.success) return result;
            temporary[it.key()] = staging;
        }
    }
    for (auto it = desired.cbegin(); it != desired.cend(); ++it)
    {
        if (it->deleted) continue;
        if (temporary.contains(it.key()))
        {
            result = GitRepository::outcome(git.run({"branch", "-m", "--", temporary.value(it.key()), it->name}));
            if (!result.success) return result;
            if (ref(oldNames.value(it.key())) == originalBranch) record.branchRef = ref(it->name);
        }
        else if (!oldNames.contains(it.key()))
        {
            result = GitRepository::outcome(git.run({"branch", "--no-track", "--", it->name, it->head}));
            if (!result.success) return result;
        }
        const auto remote = plan.directory.value(it.key());
        const auto target = remote.name.isEmpty() || remote.deleted ? it->name : remote.name;
        result = associate(git, it->name, plan.remote, target);
        if (!result.success) return result;
        if (!remote.deleted && !remote.name.isEmpty()) plan.effectiveHeads[it->name] = remote.head;
    }
    bool changed = false;
    for (auto it = desired.cbegin(); it != desired.cend(); ++it)
        if ((!it->deleted && oldNames.value(it.key()) != it->name) || (it->deleted && oldNames.contains(it.key()))) changed = true;
    record.schemeBase = plan.directory;
    record.schemes = desired;
    record.schemeEndpoint = plan.endpoint;
    record.schemeRevision = plan.revision;
    if (changed) ++record.branchVersion;
    return OperationResult::ok({});
}

OperationResult BackupSchemes::publish(BackupRecord &record, const GitRepository &git, SchemePlan &plan)
{
    if (git.remoteEndpoint(plan.remote).value != plan.endpoint) return invalid("共享仓库地址已变化，请重新同步。");
    auto directory = record.schemes;
    QMap<QString, QString> targets;
    for (auto it = directory.begin(); it != directory.end(); ++it)
    {
        if (it->deleted) continue;
        const auto head = git.resolve(ref(it->name));
        if (!head.result.success) return head.result;
        it->head = head.value;
        targets[it->name] = it->head;
        // Compare history within the same identity, not with the branch that
        // previously occupied this name (two schemes may exchange names).
        const auto before = plan.effectiveHeads.value(it->name);
        if (!before.isEmpty())
        {
            const auto fastForward = git.run({"merge-base", "--is-ancestor", before, it->head});
            if (!fastForward.success()) return invalid("方案“" + it->name + "”仍有未整理的更新，尚未发送任何方案。");
        }
    }
    auto revision = plan.revision;
    const auto data = encode(directory);
    const auto previous = revision.isEmpty() ? GitResult{} : git.run({"show", revision + ":schemes.json"});
    if (!revision.isEmpty() && previous.success() && previous.bytes == data && targets == plan.remoteHeads)
    {
        recordPublication(record, directory, plan.endpoint, revision);
        return OperationResult::ok({});
    }
    if (revision.isEmpty() || !previous.success() || previous.bytes != data)
    {
        const auto blob = git.run({"hash-object", "-w", "--stdin"}, data);
        if (!blob.success()) return GitRepository::outcome(blob);
        const auto tree = git.run({"mktree"}, ("100644 blob " + blob.output.trimmed() + "\tschemes.json\n").toUtf8());
        if (!tree.success()) return GitRepository::outcome(tree);
        QStringList args{"commit-tree", tree.output.trimmed()};
        if (!revision.isEmpty()) args << "-p" << revision;
        // Retired tips remain reachable for recovery, including a delete made
        // before the first successful shared-directory publication.
        QSet<QString> parents;
        for (const auto &scheme : directory)
            if (scheme.deleted && !parents.contains(scheme.head))
            { args << "-p" << scheme.head; parents.insert(scheme.head); }
        args << "-m" << "同步方案目录";
        const auto commit = git.run(args);
        if (!commit.success()) return GitRepository::outcome(commit);
        revision = commit.output.trimmed();
    }
    QStringList args{"push", "--atomic", "--no-follow-tags"};
    args << QString("--force-with-lease=") + DirectoryRef + ':' + plan.revision;
    QSet<QString> all;
    for (auto it = plan.remoteHeads.cbegin(); it != plan.remoteHeads.cend(); ++it) all.insert(it.key());
    for (auto it = targets.cbegin(); it != targets.cend(); ++it) all.insert(it.key());
    QStringList refspecs{revision + ':' + DirectoryRef};
    for (const auto &name : all)
    {
        args << "--force-with-lease=" + ref(name) + ':' + plan.remoteHeads.value(name);
        refspecs << targets.value(name) + ':' + ref(name);
    }
    args << "--" << plan.remote; args += refspecs;
    const auto pushed = git.run(args, {}, true, 300000);
    if (!pushed.success()) return GitRepository::outcome(pushed, "方案尚未同步");
    if (git.remoteEndpoint(plan.remote).value != plan.endpoint) return invalid("方案已发送，但共享仓库地址随后发生变化，请重新检查。");
    for (auto it = targets.cbegin(); it != targets.cend(); ++it)
        if (git.resolve(ref(it.key())).value != it.value()) return invalid("方案已发送，但本机版本随后发生变化，请重新同步。");
    for (const auto &scheme : directory)
        if (!scheme.deleted)
        {
            const auto linked = associate(git, scheme.name, plan.remote, scheme.name);
            if (!linked.success) return linked;
        }
    const auto pinned = git.run({"update-ref", "refs/zcversionbox/schemes-published", revision});
    if (!pinned.success()) return GitRepository::outcome(pinned);
    recordPublication(record, directory, plan.endpoint, revision);
    return OperationResult::ok({});
}

BackupResult<QString> BackupSchemes::merge(const GitRepository &git, const QString &repository,
                                          const QString &ours, const QString &theirs,
                                          const GitRunner &runner, std::shared_ptr<std::atomic_bool> cancel)
{
    QTemporaryDir temporary(QDir::tempPath() + "/ZcSchemeMerge-XXXXXX");
    if (!temporary.isValid()) return {OperationResult::fail("整理方案失败", "无法创建隔离合并目录。")};
    const auto path = temporary.path() + "/repository";
    auto command = git.run({"clone", "--no-hardlinks", "--no-checkout", "--", repository, path});
    if (!command.success()) return {GitRepository::outcome(command)};
    GitRepository candidate(path, runner, std::move(cancel));
    for (const auto &args : QVector<QStringList>{{"config", "user.name", "ZcVersionBox"}, {"config", "user.email", "backup@zcversionbox.local"}})
    {
        command = candidate.run(args);
        if (!command.success()) return {GitRepository::outcome(command)};
    }
    for (const auto *key : {"core.autocrlf", "core.eol", "core.filemode", "core.ignorecase", "core.attributesfile", "core.excludesfile"})
    {
        const auto value = git.run({"config", "--get", key});
        if (value.success())
        {
            command = candidate.run({"config", key, value.output.trimmed()});
            if (!command.success()) return {GitRepository::outcome(command)};
        }
        else if (value.exitCode != 1) return {GitRepository::outcome(value)};
    }
    for (const auto *name : {"attributes", "exclude"})
    {
        const auto from = repository + "/.git/info/" + name, to = path + "/.git/info/" + name;
        if (QFile::exists(from))
        {
            if (QFile::exists(to) && !QFile::remove(to)) return {OperationResult::fail("整理方案失败", "无法准备隔离仓库配置。")};
            if (!QFile::copy(from, to)) return {OperationResult::fail("整理方案失败", "无法复制仓库的文件规则。")};
        }
    }
    command = candidate.run({"checkout", "--detach", ours});
    if (!command.success()) return {GitRepository::outcome(command)};
    command = candidate.run({"merge", "--no-edit", "--no-gpg-sign", theirs});
    if (!command.success())
    {
        const auto unmerged = candidate.run({"ls-files", "-u"});
        if (unmerged.success() && !unmerged.output.isEmpty())
            return {OperationResult::info("方案内容有冲突", "双方修改了同一处内容，请选择要保留的内容。"), {}};
        return {GitRepository::outcome(command, "无法合并方案")};
    }
    const auto head = candidate.head();
    if (!head.result.success) return head;
    command = git.run({"fetch", "--no-tags", "--no-write-fetch-head", "--", path, head.value});
    return command.success() ? head : BackupResult<QString>{GitRepository::outcome(command)};
}
