#include "backup_git.h"
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>

OperationResult GitRepository::validateTagName(const QString &name) const
{
    if (name.isEmpty() || name != name.trimmed() || name.startsWith('-') || name.contains(QChar(0)) ||
        !run({"check-ref-format", "refs/tags/" + name}).success())
        return OperationResult::fail("名称不可用", "请输入名称，不要包含空格、连续句点或 ~ ^ : ? * [ \\ 等符号。");
    return OperationResult::ok({});
}
BackupResult<QMap<QString, QString>> GitRepository::tagRefs(const QString &prefix) const
{
    const auto result = run({"for-each-ref", "--format=%(refname)%00%(objectname)", prefix});
    if (!result.success()) return {outcome(result)};
    QMap<QString, QString> refs;
    for (const auto &line : result.output.split('\n', Qt::SkipEmptyParts))
    {
        const auto fields = line.split(QChar(0));
        if (fields.size() != 2 || !fields[0].startsWith(prefix))
            return {OperationResult::fail("读取标记失败", "无法确认标记引用。")};
        refs.insert(fields[0].mid(prefix.size()), fields[1]);
    }
    return {OperationResult::ok({}), refs};
}
BackupResult<QVector<VersionTag>> GitRepository::tags() const
{
    const auto refs = tagRefs();
    if (!refs.result.success) return {refs.result};
    if (refs.value.isEmpty()) return {OperationResult::ok({}), {}};
    QByteArray input;
    for (const auto &oid : refs.value) input += oid.toUtf8() + "^{commit}\n";
    const auto peeled = run({"cat-file", "--batch-check=%(objectname) %(objecttype)"}, input);
    if (!peeled.success()) return {outcome(peeled)};
    const auto lines = peeled.output.split('\n', Qt::SkipEmptyParts);
    if (lines.size() != refs.value.size()) return {OperationResult::fail("读取标记失败", "无法确认标记指向。")};
    QVector<VersionTag> tags;
    int i = 0;
    for (auto it = refs.value.cbegin(); it != refs.value.cend(); ++it, ++i)
        if (lines[i].endsWith(" commit")) tags.append({it.key(), it.value(), lines[i].section(' ', 0, 0)});
    return {OperationResult::ok({}), tags};
}
OperationResult GitRepository::updateTags(const QVector<TagRefChange> &changes) const
{
    if (changes.isEmpty()) return OperationResult::ok({});
    QByteArray input("start\n");
    const QRegularExpression oid("^(?:[0-9a-f]{40}|[0-9a-f]{64})$");
    for (const auto &change : changes)
    {
        const auto valid = validateTagName(change.name);
        if (!valid.success) return valid;
        if ((!change.before.isEmpty() && !oid.match(change.before).hasMatch()) ||
            (!change.after.isEmpty() && !oid.match(change.after).hasMatch()))
            return OperationResult::fail("标记未修改", "版本身份无效。");
        const auto ref = ("refs/tags/" + change.name).toUtf8();
        if (change.before == change.after) continue;
        if (change.before.isEmpty()) input += "create " + ref + ' ' + change.after.toUtf8() + '\n';
        else if (change.after.isEmpty()) input += "delete " + ref + ' ' + change.before.toUtf8() + '\n';
        else input += "update " + ref + ' ' + change.after.toUtf8() + ' ' + change.before.toUtf8() + '\n';
    }
    QSet<QString> retained;
    for (const auto &change : changes)
    {
        if (change.before.isEmpty()) continue;
        const auto commit = resolve(change.before);
        if (!commit.result.success || retained.contains(commit.value)) continue;
        retained.insert(commit.value);
        input += "update refs/zcversionbox-history/" + commit.value.toUtf8() + ' ' + commit.value.toUtf8() + '\n';
    }
    input += "prepare\ncommit\n";
    return outcome(run({"update-ref", "--stdin"}, input, false), "标记未修改");
}
BackupResult<QString> GitRepository::tagEndpoint() const
{
    const auto remote = remoteName();
    if (!remote.result.success) return remote;
    const auto url = run({"remote", "get-url", "--all", remote.value});
    if (!url.success()) return {OperationResult::ok({}), {}};
    const auto pushUrl = run({"remote", "get-url", "--push", "--all", remote.value});
    if (!pushUrl.success()) return {outcome(pushUrl)};
    return {OperationResult::ok({}), QString::fromLatin1(QCryptographicHash::hash(
        remote.value.toUtf8() + '\0' + url.bytes.trimmed() + '\0' + pushUrl.bytes.trimmed(), QCryptographicHash::Sha256).toHex())};
}
BackupResult<QMap<QString, QString>> GitRepository::fetchTags() const
{
    const auto endpoint = tagEndpoint(), remote = remoteName();
    if (!endpoint.result.success) return {endpoint.result};
    if (endpoint.value.isEmpty()) return {OperationResult::fail("尚未配置云端", "请先设置云端仓库地址。")};
    if (!remote.result.success) return {remote.result};
    const auto prefix = "refs/zcversionbox-tags/" + endpoint.value + '/';
    const auto fetched = run({"fetch", "--no-tags", "--no-write-fetch-head", "--prune", "--", remote.value,
                              "+refs/tags/*:" + prefix + '*'}, {}, true, 300000);
    if (!fetched.success()) return {outcome(fetched, "里程碑同步失败")};
    return tagRefs(prefix);
}
