#include "backup_merge.h"
#include "backup_catalog.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QUuid>
#include <QTimeZone>
#include <QSet>
#include <algorithm>

namespace
{
OperationResult failure(const QString &message) { return OperationResult::fail("无法处理同步差异", message); }
bool beneath(const QString &path, const QString &root) { return root == "." || path == root || path.startsWith(root + '/'); }
QJsonObject fingerprintJson(const SourceFingerprint &fingerprint)
{
    QJsonObject result;
    for (auto it = fingerprint.cbegin(); it != fingerprint.cend(); ++it) result.insert(it.key(), it.value());
    return result;
}
SourceFingerprint fingerprintValue(const QJsonObject &value)
{
    SourceFingerprint result;
    for (auto it = value.begin(); it != value.end(); ++it) result.insert(it.key(), it.value().toString());
    return result;
}
QString bytesJson(const QByteArray &bytes) { return QString::fromLatin1(bytes.toBase64()); }
QByteArray bytesValue(const QJsonValue &value) { return QByteArray::fromBase64(value.toString().toLatin1()); }
bool textBytes(const QByteArray &bytes)
{
    const auto lines = bytes.count('\n') + (!bytes.isEmpty() && !bytes.endsWith('\n') ? 1 : 0);
    if (bytes.size() > 2 * 1024 * 1024 || lines > 20000 || bytes.contains('\0')) return false;
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    // decode() is lazy: materialize the string before checking the decoder state.
    const QString decoded = decoder.decode(bytes);
    Q_UNUSED(decoded);
    return !decoder.hasError();
}
OperationResult writeBytes(const QString &path, const QByteArray &bytes)
{
    if (BackupFiles::hasLinkedAncestor(path) || !QDir().mkpath(QFileInfo(path).absolutePath())) return failure("无法准备文件：" + path);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return failure("无法保存文件：" + path);
    return OperationResult::ok({});
}
QByteArray firstLines(const QByteArray &bytes)
{
    int pos = 0;
    for (int n = 0; n < 3; ++n) { pos = bytes.indexOf('\n', pos); if (pos < 0) return bytes; ++pos; }
    return bytes.left(pos);
}
QByteArray lastLines(const QByteArray &bytes)
{
    int pos = bytes.size() - (bytes.endsWith('\n') ? 2 : 1);
    for (int n = 0; n < 3; ++n) { pos = bytes.lastIndexOf('\n', pos); if (pos < 0) return bytes; if (n < 2) --pos; }
    return bytes.mid(pos + 1);
}
#define MERGE_TRY(expression) do { const auto result = (expression); if (!result.success) return result; } while (false)
}

BackupMerge::BackupMerge(QString root, BackupDependencies dependencies, std::shared_ptr<std::atomic_bool> cancel)
    : m_root(std::move(root)), m_dependencies(std::move(dependencies)), m_cancel(std::move(cancel)) {}
GitRepository BackupMerge::git() const { return GitRepository(repositoryPath(), m_dependencies.git, m_cancel); }
QString BackupMerge::remoteCommit() const { return m_data[QLatin1String("remoteCommit")].toString(); }
QString BackupMerge::expectedHead() const { return m_data[QLatin1String("expectedHead")].toString(); }
QJsonObject BackupMerge::encodeTree(const Tree &tree)
{
    QJsonObject result;
    for (auto it = tree.cbegin(); it != tree.cend(); ++it) result.insert(it.key(), QJsonObject{{"oid", it->oid}, {"mode", it->mode}});
    return result;
}
BackupMerge::Tree BackupMerge::decodeTree(const QJsonObject &object)
{
    Tree result;
    for (auto it = object.begin(); it != object.end(); ++it) result.insert(it.key(), {it.value()[QLatin1String("oid")].toString(), it.value()[QLatin1String("mode")].toString()});
    return result;
}
BackupResult<BackupMerge::Tree> BackupMerge::tree(const QString &revision) const
{
    const auto result = git().run({"ls-tree", "-r", "-z", "--full-tree", revision});
    if (!result.success()) return {GitRepository::outcome(result)};
    Tree entries;
    for (const auto &line : result.bytes.split('\0'))
    {
        if (line.isEmpty()) continue;
        const auto tab = line.indexOf('\t');
        const auto header = line.left(tab).split(' ');
        const auto path = QString::fromUtf8(line.mid(tab + 1));
        if (tab < 0 || header.size() != 3 || path.toUtf8() != line.mid(tab + 1) || !BackupCatalog::validRepositoryPath(path) ||
            (header[0] != "100644" && header[0] != "100755"))
            return {failure("版本包含暂不支持的文件路径、符号链接或子模块。源内容已保留。")};
#ifdef Q_OS_WIN
        static const QRegularExpression reserved("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\\..*)?$", QRegularExpression::CaseInsensitiveOption);
        for (const auto &part : path.split('/'))
            if (part.endsWith('.') || part.endsWith(' ') || part.contains(QRegularExpression("[<>\"|?*\\x00-\\x1f]")) || reserved.match(part).hasMatch())
                return {failure("版本中的名称无法安全保存到此电脑：" + path)};
#endif
        entries.insert(path, {QString::fromLatin1(header[2]), QString::fromLatin1(header[0])});
    }
    return {OperationResult::ok({}), entries};
}
BackupResult<QByteArray> BackupMerge::blob(const Entry &entry) const
{
    if (entry.oid.isEmpty()) return {OperationResult::ok({}), {}};
    const auto result = git().run({"cat-file", "blob", entry.oid});
    return {GitRepository::outcome(result), result.bytes};
}
OperationResult BackupMerge::save()
{
    return writeBytes(m_root + "/session.json", QJsonDocument(m_data).toJson(QJsonDocument::Compact));
}
OperationResult BackupMerge::load()
{
    if (BackupFiles::hasLinkedAncestor(m_root)) return failure("处理记录路径已被链接替换。");
    QFile file(m_root + "/session.json");
    if (BackupFiles::hasLinkedAncestor(file.fileName()) || !file.open(QIODevice::ReadOnly)) return failure("处理记录无法读取，原文件未修改。");
    QJsonParseError error;
    const auto data = QJsonDocument::fromJson(file.readAll(), &error).object();
    if (error.error != QJsonParseError::NoError || data[QLatin1String("format")].toInt() != 1 || !BackupCatalog::validId(data[QLatin1String("id")].toString()) ||
        !BackupCatalog::validId(data[QLatin1String("backupId")].toString()) || data[QLatin1String("revision")].toString().toULongLong() == 0)
        return failure("处理记录已损坏，原文件未修改。请重新分析。");
    static const QRegularExpression oid("^[0-9a-f]{40}(?:[0-9a-f]{24})?$");
    for (const auto *name : {"localTree", "remoteTree", "automatic", "extras"})
        if (!data[name].isObject()) return failure("处理记录缺少版本内容，请重新分析。");
    if (!data["files"].isArray() || data["id"].toString() != QFileInfo(m_root).fileName() ||
        !BackupCatalog::validRepositoryPath(data["managedPath"].toString()) ||
        !data["directory"].isBool() || !data["sourceFingerprint"].isObject() || !data["capturedFingerprint"].isObject())
        return failure("处理记录结构无效，请重新分析。");
    for (const auto *name : {"localCommit", "remoteCommit", "expectedHead"})
        if (!oid.match(data[name].toString()).hasMatch()) return failure("处理记录中的版本无效。");
    for (const auto *name : {"localTree", "remoteTree", "automatic", "extras", "resultTree", "choiceLocalTree", "choiceRemoteTree"})
    {
        const auto entries = decodeTree(data[name].toObject());
        for (auto it = entries.cbegin(); it != entries.cend(); ++it)
            if (!BackupCatalog::validRepositoryPath(it.key()) || it.key() == "." || !oid.match(it->oid).hasMatch() ||
                (it->mode != "100644" && it->mode != "100755")) return failure("处理记录包含无效文件信息。");
    }
    const auto validBytes = [](const QJsonValue &value)
    {
        if (!value.isString()) return false;
        const auto encoded = value.toString().toLatin1();
        const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        return decoded && QString::fromLatin1(decoded.decoded.toBase64()) == value.toString();
    };
    QSet<QString> paths;
    for (const auto &value : data[QLatin1String("files")].toArray())
    {
        const auto path = value[QLatin1String("path")].toString();
        if (!BackupCatalog::validRepositoryPath(path) || path == "." || paths.contains(path)) return failure("处理记录中的路径无效。");
        paths.insert(path);
        const auto hunks = value[QLatin1String("hunks")].toArray(), common = value[QLatin1String("common")].toArray();
        if (!value[QLatin1String("whole")].isBool() || hunks.isEmpty() ||
            (value[QLatin1String("whole")].toBool() ? hunks.size() != 1 : !value[QLatin1String("common")].isArray() || common.size() != hunks.size() + 1))
            return failure("处理记录中的差异片段不完整，请重新分析。");
        for (const auto &hunk : hunks)
        {
            if (hunk[QLatin1String("choice")].toInt(-1) < 0 || hunk[QLatin1String("choice")].toInt(-1) > 2) return failure("处理记录中的选择无效。");
            if (!value[QLatin1String("whole")].toBool() && (!validBytes(hunk[QLatin1String("local")]) || !validBytes(hunk[QLatin1String("remote")])))
                return failure("处理记录中的文本内容已损坏，请重新分析。");
        }
        if (!value[QLatin1String("whole")].toBool())
        {
            if (value[QLatin1String("mode")] != "100644" && value[QLatin1String("mode")] != "100755") return failure("处理记录中的文件权限无效。");
            for (const auto &part : common)
                if (!validBytes(part)) return failure("处理记录中的上下文已损坏，请重新分析。");
        }
    }
    m_data = data;
    return git().validate();
}
OperationResult BackupMerge::create(const BackupRecord &record, const QString &liveRepository, const QString &pinnedRemote, const QString &mergeRef)
{
    if (BackupFiles::exists(m_root) || BackupFiles::hasLinkedAncestor(m_root) || !QDir().mkpath(m_root)) return failure("无法创建独立处理目录。");
    GitRepository live(liveRepository, m_dependencies.git, m_cancel);
    MERGE_TRY(live.clean());
    const auto head = live.head(), remote = live.remoteName(), ref = live.targetRef();
    if (!head.result.success || !remote.result.success || !ref.result.success) return failure("无法确定同步版本或云端配置。");
    const auto url = mergeRef.isEmpty() ? live.run({"remote", "get-url", remote.value}) : GitResult{};
    if (mergeRef.isEmpty() && !url.success()) return failure("尚未配置可用的云端地址。");
    if (!mergeRef.isEmpty() && (!mergeRef.startsWith("refs/heads/") || live.resolve(mergeRef).value != pinnedRemote))
        return failure("待合并方案已变化，请重新选择。");
    SourceFingerprint before, ignored;
    MERGE_TRY(m_dependencies.files->fingerprint(record.sourcePath, record.directory, before, false));
    // Keep the original basename so full fingerprints for single files are stable.
    const auto captured = m_root + "/source/" + QFileInfo(record.sourcePath).fileName();
    MERGE_TRY(m_dependencies.files->capture(record.sourcePath, record.directory, captured, ignored));
    SourceFingerprint capturedFingerprint;
    MERGE_TRY(m_dependencies.files->fingerprint(captured, record.directory, capturedFingerprint, false));
    const auto cloned = live.run({"clone", "--no-hardlinks", "--no-checkout", "--", liveRepository, repositoryPath()});
    MERGE_TRY(GitRepository::outcome(cloned));
    MERGE_TRY(GitRepository::outcome(git().run({"config", "user.name", "ZcVersionBox"})));
    MERGE_TRY(GitRepository::outcome(git().run({"config", "user.email", "backup@zcversionbox.local"})));
    // Source capture must obey the same normalization and ignore rules as the
    // live backup; clone does not copy repository-local configuration/info.
    for (const auto *key : {"core.autocrlf", "core.eol", "core.filemode", "core.ignorecase", "core.attributesfile", "core.excludesfile"})
    {
        const auto value = live.run({"config", "--get", key});
        if (value.success()) MERGE_TRY(GitRepository::outcome(git().run({"config", key, value.output.trimmed()})));
        else if (value.exitCode != 1) return GitRepository::outcome(value);
    }
    for (const auto *name : {"exclude", "attributes"})
    {
        const auto from = liveRepository + "/.git/info/" + name, to = repositoryPath() + "/.git/info/" + name;
        if (!BackupFiles::exists(from)) continue;
        MERGE_TRY(m_dependencies.files->remove(to));
        MERGE_TRY(m_dependencies.files->copy(from, to));
    }
    auto remoteCommit = pinnedRemote;
    if (remoteCommit.isEmpty())
    {
        MERGE_TRY(GitRepository::outcome(git().run({"fetch", "--no-tags", "--", url.output.trimmed(), ref.value}, {}, true, 300000), "获取云端版本失败"));
        const auto fetched = git().resolve("FETCH_HEAD");
        if (!fetched.result.success) return fetched.result;
        remoteCommit = fetched.value;
    }
    const auto localTree = tree(record.lastCommit), remoteTree = tree(remoteCommit);
    if (!localTree.result.success) return localTree.result;
    if (!remoteTree.result.success) return remoteTree.result;
    MERGE_TRY(git().validateMapping(remoteCommit, record.repositoryPath, record.directory));
    if (!git().run({"merge-base", record.lastCommit, remoteCommit}).success()) return failure("两个版本没有共同历史，暂不能合并。请检查所选方案或云端地址。");
    MERGE_TRY(GitRepository::outcome(git().run({"checkout", "--detach", record.lastCommit})));
    const auto managed = record.repositoryPath == "." ? repositoryPath() : repositoryPath() + '/' + record.repositoryPath;
    // Replacement preserves the candidate's .git directory for a root mapping.
    BackupReplacement replacement(m_dependencies.files, managed, m_root);
    if (!replacement.valid()) return failure("无法准备本地快照。");
    MERGE_TRY(m_dependencies.files->copy(captured, replacement.stagingPath()));
    MERGE_TRY(replacement.install());
    MERGE_TRY(replacement.finish());
    MERGE_TRY(GitRepository::outcome(git().run({"add", "--all", "--", record.repositoryPath})));
    const auto difference = git().run({"diff", "--cached", "--quiet"});
    if (!difference.started || !difference.finished || (difference.exitCode != 0 && difference.exitCode != 1)) return GitRepository::outcome(difference);
    if (difference.exitCode == 1)
        MERGE_TRY(GitRepository::outcome(git().run({"commit", "-m", "同步前的本地版本"})));
    const auto local = git().head();
    if (!local.result.success) return local.result;
    const auto sourceTree = tree(local.value);
    if (!sourceTree.result.success) return sourceTree.result;
    Tree extras;
    const auto untracked = git().run({"ls-files", "--others", "--ignored", "--exclude-standard", "-z", "--", record.repositoryPath});
    MERGE_TRY(GitRepository::outcome(untracked));
    for (const auto &name : untracked.bytes.split('\0'))
    {
        if (name.isEmpty()) continue;
        const auto path = QString::fromUtf8(name);
        if (!BackupCatalog::validRepositoryPath(path) || name != path.toUtf8()) return failure("忽略文件的路径无法读取。");
        QFile file(repositoryPath() + '/' + path);
        if (!file.open(QIODevice::ReadOnly)) return failure("无法保留本地忽略文件：" + path);
        const auto object = git().run({"hash-object", "-w", "--stdin"}, file.readAll());
        MERGE_TRY(GitRepository::outcome(object));
        extras.insert(path, {object.output.trimmed(), QFileInfo(file).permission(QFileDevice::ExeOwner) ? "100755" : "100644"});
    }
    if (mergeRef.isEmpty()) MERGE_TRY(writeBytes(repositoryPath() + "/.git/info/attributes", "* -merge\n"));
    m_data = {{"format", 1}, {"id", QFileInfo(m_root).fileName()}, {"backupId", record.id}, {"revision", "1"},
              {"generation", QString::number(record.generation)}, {"sourcePath", record.sourcePath}, {"managedPath", record.repositoryPath},
              {"directory", record.directory}, {"sourceFingerprint", fingerprintJson(before)}, {"capturedFingerprint", fingerprintJson(capturedFingerprint)},
              {"localCommit", local.value}, {"remoteCommit", remoteCommit}, {"expectedHead", head.value}, {"remoteName", remote.value},
              {"remoteUrl", url.output.trimmed()}, {"remoteRef", ref.value}, {"localTree", encodeTree(sourceTree.value)}, {"remoteTree", encodeTree(remoteTree.value)},
              {"extras", encodeTree(extras)}, {"localTime", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    m_data["expectedBranch"] = live.branchRef().value;
    m_data["branchVersion"] = QString::number(record.branchVersion);
    m_data["mergeRef"] = mergeRef;
    m_data["localLabel"] = mergeRef.isEmpty() ? QStringLiteral("此电脑上的内容 (本地)") : record.branchRef.mid(11);
    m_data["remoteLabel"] = mergeRef.isEmpty() ? QStringLiteral("云端的内容 (云端)") : mergeRef.mid(11);
    const auto date = git().run({"show", "-s", "--format=%ct", remoteCommit});
    MERGE_TRY(GitRepository::outcome(date));
    m_data[QLatin1String("remoteTime")] = QDateTime::fromSecsSinceEpoch(date.output.trimmed().toLongLong(), QTimeZone::UTC).toString(Qt::ISODateWithMs);
    MERGE_TRY(analyze());
    MERGE_TRY(verify(record, liveRepository));
    return save();
}

OperationResult BackupMerge::mergeText(QJsonObject &file, const Entry &base, const Entry &local, const Entry &remote)
{
    // Check object sizes before loading blobs: whole-file choices must also be
    // usable for multi-gigabyte files without allocating their contents here.
    for (const auto &entry : {base, local, remote})
    {
        if (entry.oid.isEmpty()) continue;
        const auto size = git().run({"cat-file", "-s", entry.oid});
        MERGE_TRY(GitRepository::outcome(size));
        if (size.output.trimmed().toLongLong() > 2 * 1024 * 1024) return OperationResult::ok({});
    }
    const auto b = blob(base), l = blob(local), r = blob(remote);
    if (!b.result.success) return b.result;
    if (!l.result.success) return l.result;
    if (!r.result.success) return r.result;
    if (!textBytes(b.value) || !textBytes(l.value) || !textBytes(r.value)) return OperationResult::ok({});
    int marker = 32;
    for (const auto &bytes : {b.value, l.value, r.value})
        for (char ch : {'<', '>', '|', '='})
            while (bytes.contains(QByteArray(marker, ch))) ++marker;
    MERGE_TRY(writeBytes(m_root + "/base", b.value));
    MERGE_TRY(writeBytes(m_root + "/local", l.value));
    MERGE_TRY(writeBytes(m_root + "/remote", r.value));
    const auto merged = git().run({"merge-file", "-p", "--diff3", "--marker-size=" + QString::number(marker), "-L", "local", "-L", "base", "-L", "remote", m_root + "/local", m_root + "/base", m_root + "/remote"});
    // merge-file uses a positive conflict count (capped at 127), not a failure status.
    if (!merged.started || !merged.finished || merged.exitCode < 0 || merged.exitCode > 127) return GitRepository::outcome(merged);
    const QByteArray markerEol = merged.bytes.contains(QByteArray(marker, '<') + " local\r\n") ? "\r\n" : "\n";
    const auto start = QByteArray(marker, '<') + " local" + markerEol, middle = QByteArray(marker, '|') + " base" + markerEol,
               separator = QByteArray(marker, '=') + markerEol, end = QByteArray(marker, '>') + " remote" + markerEol;
    QJsonArray common, hunks;
    const auto &output = merged.bytes;
    qsizetype cursor = 0;
    while (true)
    {
        const auto first = output.indexOf(start, cursor);
        if (first < 0) break;
        const auto second = output.indexOf(middle, first + start.size());
        const auto third = output.indexOf(separator, second + middle.size());
        const auto fourth = output.indexOf(end, third + separator.size());
        if ((first && output[first - 1] != '\n') || second < 0 || third < 0 || fourth < 0) return failure("无法安全识别文本差异。");
        common.append(bytesJson(output.mid(cursor, first - cursor)));
        auto localPart = output.mid(first + start.size(), second - first - start.size());
        auto remotePart = output.mid(third + separator.size(), fourth - third - separator.size());
        cursor = fourth + end.size();
        // Git inserts a separator newline around an EOF conflict. It is not file content.
        if (cursor == output.size())
        {
            if (!l.value.endsWith('\n') && localPart.endsWith('\n') && l.value.endsWith(localPart.chopped(1))) localPart.chop(1);
            if (!r.value.endsWith('\n') && remotePart.endsWith('\n') && r.value.endsWith(remotePart.chopped(1))) remotePart.chop(1);
        }
        hunks.append(QJsonObject{{"local", bytesJson(localPart)}, {"remote", bytesJson(remotePart)}, {"choice", 0}});
    }
    common.append(bytesJson(output.mid(cursor)));
    if ((merged.exitCode == 0) != hunks.isEmpty()) return failure("文本差异数量无法确认。");
    file[QLatin1String("whole")] = false;
    file[QLatin1String("common")] = common;
    file[QLatin1String("hunks")] = hunks;
    return OperationResult::ok({});
}
OperationResult BackupMerge::analyze()
{
    QStringList mergeArgs{"merge", "--no-commit", "--no-ff", "--no-edit"};
    if (m_data["mergeRef"].toString().isEmpty()) mergeArgs << "-s" << "recursive" << "-Xno-renames";
    mergeArgs << remoteCommit();
    const auto merged = git().run(mergeArgs);
    if (!merged.started || !merged.finished || (merged.exitCode != 0 && merged.exitCode != 1)) return GitRepository::outcome(merged);
    const auto staged = git().run({"ls-files", "--stage", "-z"});
    MERGE_TRY(GitRepository::outcome(staged));
    Tree automatic, bases, localStages, remoteStages;
    QStringList unresolved;
    for (const auto &line : staged.bytes.split('\0'))
    {
        if (line.isEmpty()) continue;
        const auto tab = line.indexOf('\t');
        const auto header = line.left(tab).split(' ');
        if (tab < 0 || header.size() != 3) return failure("无法读取候选版本。");
        const auto path = QString::fromUtf8(line.mid(tab + 1));
        const Entry entry{QString::fromLatin1(header[1]), QString::fromLatin1(header[0])};
        const int stage = header[2].toInt();
        if (stage == 0) automatic.insert(path, entry);
        else
        {
            if (stage == 1) bases.insert(path, entry);
            if (stage == 2) localStages.insert(path, entry);
            if (stage == 3) remoteStages.insert(path, entry);
            if (!unresolved.contains(path)) unresolved.append(path);
        }
    }
    auto local = decodeTree(m_data[QLatin1String("localTree")].toObject()), remote = decodeTree(m_data[QLatin1String("remoteTree")].toObject());
    const auto extras = decodeTree(m_data[QLatin1String("extras")].toObject());
    if (!m_data["mergeRef"].toString().isEmpty())
    {
        // Git may move the conflict's two sides to a rename destination. Use
        // the actual index stages there, while retaining the original trees
        // separately for preview and native fast-forward checks.
        for (const auto &path : unresolved)
        {
            if (!local.contains(path) && !remote.contains(path)) continue; // structural ~HEAD paths use their parent group
            if (localStages.contains(path)) local.insert(path, localStages[path]); else local.remove(path);
            if (remoteStages.contains(path)) remote.insert(path, remoteStages[path]); else remote.remove(path);
        }
        m_data["choiceLocalTree"] = encodeTree(local); m_data["choiceRemoteTree"] = encodeTree(remote);
    }
    Tree localWithExtras = local;
    for (auto it = extras.cbegin(); it != extras.cend(); ++it) localWithExtras.insert(it.key(), it.value());
    QStringList groups;
    // A file vs directory occupies one logical path; do not expose Git's temporary ~HEAD names.
    for (const auto &side : {localWithExtras, remote})
        for (auto it = side.cbegin(); it != side.cend(); ++it)
            for (const auto &other : {localWithExtras, remote})
            {
                auto next = other.lowerBound(it.key() + '/');
                if (next != other.cend() && next.key().startsWith(it.key() + '/')) groups.append(it.key());
            }
    for (const auto &path : unresolved)
        if (local.contains(path) || remote.contains(path)) groups.append(path);
    for (auto it = extras.cbegin(); it != extras.cend(); ++it)
        if (remote.contains(it.key()) && !(remote[it.key()] == it.value())) groups.append(it.key());
    groups.removeDuplicates();
    std::sort(groups.begin(), groups.end());
    QStringList roots;
    for (const auto &path : groups)
        if (std::none_of(roots.cbegin(), roots.cend(), [&](const QString &root) { return beneath(path, root); })) roots.append(path);
    // Any unrecognized conflict must not turn into silent deletion.
    for (const auto &path : unresolved)
        if (!local.contains(path) && !remote.contains(path) && roots.isEmpty()) return failure("此版本包含暂不支持的结构冲突。");
    for (auto it = automatic.begin(); it != automatic.end();)
        if ((!local.contains(it.key()) && !remote.contains(it.key())) || std::any_of(roots.cbegin(), roots.cend(), [&](const QString &root) { return beneath(it.key(), root); })) it = automatic.erase(it);
        else ++it;
    QJsonArray files;
    for (const auto &path : roots)
    {
        QJsonObject file{{"path", path}, {"whole", true}, {"hunks", QJsonArray{QJsonObject{{"choice", 0}}}}};
        file[QLatin1String("mode")] = local.value(path).mode == remote.value(path).mode ? local.value(path).mode
            : bases.value(path).mode == local.value(path).mode ? remote.value(path).mode : local.value(path).mode;
        const auto size = [&](const Tree &side)
        {
            qint64 total = 0;
            for (auto it = side.cbegin(); it != side.cend(); ++it) if (beneath(it.key(), path))
            { const auto result = git().run({"cat-file", "-s", it->oid}); if (result.success()) total += result.output.trimmed().toLongLong(); }
            return total;
        };
        const auto exists = [&](const Tree &side)
        { return std::any_of(side.keyBegin(), side.keyEnd(), [&](const QString &key) { return beneath(key, path); }); };
        file[QLatin1String("localExists")] = exists(localWithExtras); file[QLatin1String("remoteExists")] = exists(remote);
        file[QLatin1String("localDirectory")] = exists(localWithExtras) && !localWithExtras.contains(path);
        file[QLatin1String("remoteDirectory")] = exists(remote) && !remote.contains(path);
        file[QLatin1String("localSize")] = QString::number(size(localWithExtras)); file[QLatin1String("remoteSize")] = QString::number(size(remote));
        file[QLatin1String("managed")] = beneath(path, m_data[QLatin1String("managedPath")].toString());
        if (localWithExtras.contains(path) && remote.contains(path) && !extras.contains(path))
            MERGE_TRY(mergeText(file, bases.value(path), localWithExtras[path], remote[path]));
        if (file[QLatin1String("hunks")].toArray().isEmpty())
        {
            const auto data = bytesValue(file[QLatin1String("common")].toArray().first());
            const auto object = git().run({"hash-object", "-w", "--stdin"}, data);
            MERGE_TRY(GitRepository::outcome(object));
            automatic.insert(path, {object.output.trimmed(), local[path].mode == remote[path].mode ? local[path].mode : (bases.value(path).mode == local[path].mode ? remote[path].mode : local[path].mode)});
        }
        else files.append(file);
    }
    m_data[QLatin1String("automatic")] = encodeTree(automatic);
    m_data[QLatin1String("files")] = files;
    return OperationResult::ok({});
}
SyncResolutionSession BackupMerge::session() const
{
    SyncResolutionSession result;
    result.id = m_data[QLatin1String("id")].toString(); result.backupId = m_data[QLatin1String("backupId")].toString(); result.revision = m_data[QLatin1String("revision")].toString().toULongLong();
    result.localTime = QDateTime::fromString(m_data[QLatin1String("localTime")].toString(), Qt::ISODateWithMs);
    result.remoteTime = QDateTime::fromString(m_data[QLatin1String("remoteTime")].toString(), Qt::ISODateWithMs);
    result.currentPath = m_data[QLatin1String("currentPath")].toString();
    result.currentHunk = m_data[QLatin1String("currentHunk")].toInt();
    result.mergeRef = m_data["mergeRef"].toString();
    result.localLabel = m_data["localLabel"].toString("此电脑上的内容 (本地)");
    result.remoteLabel = m_data["remoteLabel"].toString("云端的内容 (云端)");
    for (const auto &value : m_data[QLatin1String("files")].toArray())
    {
        ConflictFile f;
        f.path = value[QLatin1String("path")].toString(); f.wholeFile = value[QLatin1String("whole")].toBool(); f.managed = value[QLatin1String("managed")].toBool();
        f.localExists = value[QLatin1String("localExists")].toBool(); f.remoteExists = value[QLatin1String("remoteExists")].toBool();
        f.localDirectory = value[QLatin1String("localDirectory")].toBool(); f.remoteDirectory = value[QLatin1String("remoteDirectory")].toBool();
        f.localSize = value[QLatin1String("localSize")].toString().toLongLong(); f.remoteSize = value[QLatin1String("remoteSize")].toString().toLongLong();
        const auto common = value[QLatin1String("common")].toArray(), hunks = value[QLatin1String("hunks")].toArray();
        for (int i = 0; i < hunks.size(); ++i)
            f.hunks.append({bytesValue(hunks[i][QLatin1String("local")]), bytesValue(hunks[i][QLatin1String("remote")]), lastLines(bytesValue((i < common.size() ? common[i] : QJsonValue()))), firstLines(bytesValue((i + 1 < common.size() ? common[i + 1] : QJsonValue()))), static_cast<ConflictChoice>(hunks[i][QLatin1String("choice")].toInt())});
        result.files.append(f);
    }
    return result;
}
OperationResult BackupMerge::verify(const BackupRecord &record, const QString &liveRepository) const
{
    if (record.id != m_data[QLatin1String("backupId")].toString() || record.generation != m_data[QLatin1String("generation")].toString().toULongLong() ||
        record.sourcePath != m_data[QLatin1String("sourcePath")].toString() || record.repositoryPath != m_data[QLatin1String("managedPath")].toString()) return failure("备份对象已变化，请重新分析。");
    GitRepository live(liveRepository, m_dependencies.git, m_cancel);
    MERGE_TRY(live.validate());
    MERGE_TRY(live.clean());
    const auto head = live.head(), remote = live.remoteName(), ref = live.targetRef();
    if (!head.result.success || head.value != expectedHead()) return failure("备份版本在处理期间发生变化，请检查后重新分析。");
    const auto expectedBranch = m_data["expectedBranch"].toString(record.branchRef);
    if (live.branchRef().value != expectedBranch || (!m_data["branchVersion"].isUndefined() && record.branchVersion != m_data["branchVersion"].toString().toULongLong()))
        return failure("当前方案在处理期间发生变化，请重新分析。");
    const auto mergeRef = m_data["mergeRef"].toString();
    if (!mergeRef.isEmpty())
    {
        if (!mergeRef.startsWith("refs/heads/") || live.resolve(mergeRef).value != remoteCommit())
            return failure("待合并方案在处理期间发生变化，请重新分析。");
    }
    else
    {
        if (!remote.result.success || !ref.result.success || remote.value != m_data[QLatin1String("remoteName")].toString() || ref.value != m_data[QLatin1String("remoteRef")].toString()) return failure("同步配置已变化，请重新分析。");
        const auto url = live.run({"remote", "get-url", remote.value});
        if (!url.success() || url.output.trimmed() != m_data[QLatin1String("remoteUrl")].toString()) return failure("云端地址已变化，请重新分析。");
    }
    SourceFingerprint current;
    MERGE_TRY(m_dependencies.files->fingerprint(record.sourcePath, record.directory, current, false));
    if (current != fingerprintValue(m_data[QLatin1String("sourceFingerprint")].toObject())) return failure("源内容在处理期间发生变化，已保留选择。请重新分析后再应用。");
    MERGE_TRY(m_dependencies.files->fingerprint(m_root + "/source/" + QFileInfo(record.sourcePath).fileName(), record.directory, current, false));
    if (current != fingerprintValue(m_data[QLatin1String("capturedFingerprint")].toObject())) return failure("处理副本发生变化，请重新分析。");
    return OperationResult::ok({});
}
OperationResult BackupMerge::choose(quint64 revision, const QString &path, int hunk, ConflictChoice choice)
{
    if (revision != session().revision || choice == ConflictChoice::Unresolved || int(choice) < 0 || int(choice) > 2) return failure("处理进度已变化，请刷新后重试。");
    const auto before = m_data;
    auto files = m_data[QLatin1String("files")].toArray();
    for (int i = 0; i < files.size(); ++i)
    {
        auto file = files[i].toObject();
        if (file[QLatin1String("path")].toString() != path) continue;
        auto hunks = file[QLatin1String("hunks")].toArray();
        if (hunk < 0 || hunk >= hunks.size()) return failure("此处差异已失效。");
        auto value = hunks[hunk].toObject(); value[QLatin1String("choice")] = int(choice); hunks[hunk] = value;
        file[QLatin1String("hunks")] = hunks; files[i] = file; m_data[QLatin1String("files")] = files;
        m_data[QLatin1String("currentPath")] = path;
        m_data[QLatin1String("currentHunk")] = hunk;
        m_data[QLatin1String("revision")] = QString::number(revision + 1); m_data.remove("resultCommit"); m_data.remove("resultTree"); m_data.remove("treeOid");
        const auto saved = save();
        if (!saved.success) m_data = before;
        return saved;
    }
    return failure("此文件已不在处理列表中。");
}
BackupResult<BackupMerge::Tree> BackupMerge::resultTree() const
{
    if (session().remaining()) return {failure("还有内容尚未选择。")};
    auto result = decodeTree(m_data[QLatin1String("automatic")].toObject());
    auto local = decodeTree(m_data[m_data.contains("choiceLocalTree") ? "choiceLocalTree" : "localTree"].toObject());
    const auto remote = decodeTree(m_data[m_data.contains("choiceRemoteTree") ? "choiceRemoteTree" : "remoteTree"].toObject()), extras = decodeTree(m_data[QLatin1String("extras")].toObject());
    for (auto it = extras.cbegin(); it != extras.cend(); ++it) local.insert(it.key(), it.value());
    for (const auto &file : m_data[QLatin1String("files")].toArray())
    {
        const auto path = file[QLatin1String("path")].toString(); const auto hunks = file[QLatin1String("hunks")].toArray();
        if (file[QLatin1String("whole")].toBool())
        {
            const auto &side = hunks[0][QLatin1String("choice")].toInt() == int(ConflictChoice::Local) ? local : remote;
            for (auto it = side.cbegin(); it != side.cend(); ++it) if (beneath(it.key(), path)) result.insert(it.key(), it.value());
        }
        else
        {
            const auto common = file[QLatin1String("common")].toArray(); QByteArray bytes;
            for (int i = 0; i < hunks.size(); ++i)
            { bytes += bytesValue(common[i]); bytes += bytesValue(hunks[i].toObject()[hunks[i][QLatin1String("choice")].toInt() == int(ConflictChoice::Local) ? "local" : "remote"]); }
            bytes += bytesValue(common.last());
            const auto object = git().run({"hash-object", "-w", "--stdin"}, bytes);
            if (!object.success()) return {GitRepository::outcome(object)};
            result.insert(path, {object.output.trimmed(), file[QLatin1String("mode")].toString(local.value(path).mode)});
        }
    }
    return {OperationResult::ok({}), result};
}
BackupResult<PreparedSyncApply> BackupMerge::prepare()
{
    const auto assembled = resultTree();
    if (!assembled.result.success) return {assembled.result};
    const auto reset = git().run({"read-tree", "--empty"});
    if (!reset.success()) return {GitRepository::outcome(reset)};
    QByteArray index;
    for (auto it = assembled.value.cbegin(); it != assembled.value.cend(); ++it) index += it->mode.toUtf8() + ' ' + it->oid.toUtf8() + '\t' + it.key().toUtf8() + '\0';
    const auto staged = git().run({"update-index", "-z", "--index-info"}, index);
    if (!staged.success()) return {GitRepository::outcome(staged)};
    const auto treeOid = git().run({"write-tree"});
    if (!treeOid.success()) return {GitRepository::outcome(treeOid)};
    const auto local = m_data[QLatin1String("localCommit")].toString(), remote = remoteCommit();
    const auto remoteContainsLocal = git().run({"merge-base", "--is-ancestor", local, remote});
    const auto localContainsRemote = git().run({"merge-base", "--is-ancestor", remote, local});
    for (const auto &r : {remoteContainsLocal, localContainsRemote})
        if (!r.started || !r.finished || (r.exitCode != 0 && r.exitCode != 1)) return {GitRepository::outcome(r)};
    QString commit;
    if (remoteContainsLocal.success() && encodeTree(assembled.value) == m_data[QLatin1String("remoteTree")].toObject()) commit = remote;
    else if (localContainsRemote.success() && encodeTree(assembled.value) == m_data[QLatin1String("localTree")].toObject()) commit = local;
    else
    {
        QStringList args{"commit-tree", treeOid.output.trimmed(), "-p", local};
        if (!localContainsRemote.success()) args << "-p" << remote;
        args << "-m" << (m_data["mergeRef"].toString().isEmpty() ? QStringLiteral("合并本地与云端修改")
                         : QString("合并方案 %1 到 %2").arg(session().remoteLabel, session().localLabel));
        const auto created = git().run(args);
        if (!created.success()) return {GitRepository::outcome(created)};
        commit = created.output.trimmed();
    }
    const auto retained = git().run({"branch", "-f", "resolution-result", commit});
    if (!retained.success()) return {GitRepository::outcome(retained)};
    const auto mapping = git().validateMapping(commit, m_data[QLatin1String("managedPath")].toString(), m_data[QLatin1String("directory")].toBool());
    if (!mapping.success) return {mapping};
    m_data[QLatin1String("resultCommit")] = commit; m_data[QLatin1String("resultTree")] = encodeTree(assembled.value); m_data[QLatin1String("treeOid")] = treeOid.output.trimmed();
    const auto saved = save();
    if (!saved.success) return {saved};
    PreparedSyncApply request{m_data[QLatin1String("backupId")].toString(), m_data[QLatin1String("id")].toString(), commit, treeOid.output.trimmed(), session().revision,
                             m_data[QLatin1String("generation")].toString().toULongLong(), fingerprintValue(m_data[QLatin1String("sourceFingerprint")].toObject()), {}};
    const auto localRepository = decodeTree(m_data[QLatin1String("localTree")].toObject());
    auto localSource = localRepository;
    const auto finalSource = withRetainedExtras(assembled.value);
    const auto extras = decodeTree(m_data[QLatin1String("extras")].toObject());
    for (auto it = extras.cbegin(); it != extras.cend(); ++it)
    {
        localSource.insert(it.key(), it.value());
    }
    // Include repository-only changes as well as the changes to current source.
    QSet<QString> paths;
    for (const auto &path : localSource.keys()) paths.insert(path);
    for (const auto &path : finalSource.keys()) paths.insert(path);
    QStringList ordered = paths.values(); std::sort(ordered.begin(), ordered.end());
    for (const auto &path : ordered)
        if (!localSource.contains(path) || !finalSource.contains(path) || !(localSource[path] == finalSource[path]))
            request.changes.append({!localSource.contains(path) ? "A" : !finalSource.contains(path) ? "D" : "M", path,
                                    beneath(path, m_data[QLatin1String("managedPath")].toString()) ? QString() : "仅影响备份仓库"});
        else if (!localRepository.contains(path) && assembled.value.contains(path))
            request.changes.append({"A", path, "源内容不变，新增到备份仓库"});
    return {OperationResult::ok({}), request};
}
bool BackupMerge::validRequest(const PreparedSyncApply &request) const
{
    return request.id == m_data[QLatin1String("backupId")].toString() && request.sessionId == session().id && request.revision == session().revision &&
           request.generation == m_data[QLatin1String("generation")].toString().toULongLong() && !request.commit.isEmpty() && request.commit == m_data[QLatin1String("resultCommit")].toString() &&
           request.tree == m_data[QLatin1String("treeOid")].toString() && request.sourceFingerprint == fingerprintValue(m_data[QLatin1String("sourceFingerprint")].toObject());
}
OperationResult BackupMerge::exportTree(const Tree &entries, const QString &prefix, const QString &target) const
{
    if (!BackupCatalog::validRepositoryPath(prefix) || BackupFiles::hasLinkedAncestor(target)) return failure("导出路径无效。");
    const bool single = entries.contains(prefix);
    if (!single && !QDir().mkpath(target)) return failure("无法创建文件夹。");
    for (auto it = entries.cbegin(); it != entries.cend(); ++it)
    {
        if (!beneath(it.key(), prefix)) continue;
        const auto path = single ? target : QDir(target).filePath(prefix == "." ? it.key() : it.key().mid(prefix.size() + 1));
        if (BackupFiles::exists(path) || BackupFiles::hasLinkedAncestor(path)) return failure("文件路径存在碰撞，已停止应用：" + it.key());
        const auto bytes = blob(it.value());
        if (!bytes.result.success) return bytes.result;
        MERGE_TRY(writeBytes(path, bytes.value));
        auto permissions = QFile::permissions(path);
        permissions &= ~(QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther);
        if (it->mode == "100755") permissions |= QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther;
        if (!QFile::setPermissions(path, permissions)) return failure("无法设置文件执行权限：" + it.key());
    }
    return OperationResult::ok({});
}
OperationResult BackupMerge::exportSource(const PreparedSyncApply &request, const QString &target) const
{
    if (!validRequest(request)) return failure("结果预览已失效，请重新预览。");
    return exportTree(sideTree(ConflictSide::Result), m_data[QLatin1String("managedPath")].toString(), target);
}
BackupMerge::Tree BackupMerge::withRetainedExtras(Tree entries) const
{
    const auto extras = decodeTree(m_data[QLatin1String("extras")].toObject());
    for (auto it = extras.cbegin(); it != extras.cend(); ++it)
    {
        bool chosen = false;
        for (const auto &file : m_data[QLatin1String("files")].toArray()) if (beneath(it.key(), file[QLatin1String("path")].toString())) { chosen = true; break; }
        if (!chosen && !entries.contains(it.key())) entries.insert(it.key(), it.value());
    }
    return entries;
}
BackupMerge::Tree BackupMerge::sideTree(ConflictSide side) const
{
    const auto key = side == ConflictSide::Result ? "resultTree" : side == ConflictSide::Local
        ? (m_data.contains("choiceLocalTree") ? "choiceLocalTree" : "localTree")
        : (m_data.contains("choiceRemoteTree") ? "choiceRemoteTree" : "remoteTree");
    auto entries = decodeTree(m_data[key].toObject());
    if (side == ConflictSide::Local)
    { const auto extras = decodeTree(m_data[QLatin1String("extras")].toObject()); for (auto it = extras.cbegin(); it != extras.cend(); ++it) entries.insert(it.key(), it.value()); }
    if (side == ConflictSide::Result) entries = withRetainedExtras(std::move(entries));
    return entries;
}
BackupResult<ConflictContent> BackupMerge::content(const QString &path, ConflictSide side) const
{
    const auto entries = sideTree(side);
    if (!entries.contains(path)) return {OperationResult::ok({}), {"此版本中没有该文件，或此项为文件夹。", false}};
    const auto size = git().run({"cat-file", "-s", entries[path].oid});
    if (!size.success()) return {GitRepository::outcome(size)};
    if (size.output.trimmed().toLongLong() > 2 * 1024 * 1024) return {OperationResult::ok({}), {"文件较大，请打开只读副本预览。", false}};
    const auto bytes = blob(entries[path]);
    if (!bytes.result.success) return {bytes.result};
    const auto textual = textBytes(bytes.value);
    return {OperationResult::ok({}), {textual ? QString::fromUtf8(bytes.value) : "此文件不能按文本显示，请打开只读副本预览。", textual}};
}
OperationResult BackupMerge::exportSide(const QString &path, ConflictSide side, const QString &target) const
{
    const auto entries = sideTree(side);
    if (std::none_of(entries.keyBegin(), entries.keyEnd(), [&](const QString &entry) { return beneath(entry, path); })) return failure("此版本中不存在该文件。");
    return exportTree(entries, path, target);
}
