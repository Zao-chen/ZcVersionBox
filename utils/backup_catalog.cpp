#include "backup_catalog.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>
#include <QUuid>

namespace
{
QJsonObject tagMapJson(const QMap<QString, QString> &map)
{
    QJsonObject json;
    for (auto it = map.cbegin(); it != map.cend(); ++it) json.insert(it.key(), it.value());
    return json;
}
bool readTagMap(const QJsonValue &value, QMap<QString, QString> &map)
{
    if (!value.isObject()) return false;
    static const QRegularExpression oid("^(?:[0-9a-f]{40}|[0-9a-f]{64})?$");
    const auto json = value.toObject();
    for (auto it = json.begin(); it != json.end(); ++it)
    {
        if (it.key().isEmpty() || !it.value().isString() || !oid.match(it.value().toString()).hasMatch()) return false;
        map.insert(it.key(), it.value().toString());
    }
    return true;
}
bool readTagState(const QJsonValue &value, BackupRecord &record)
{
    if (value.isUndefined()) return true;
    if (!value.isObject()) return false;
    const auto state = value.toObject();
    if (state["format"].toInt() != 1 || !state["remotes"].isObject() || !state["journal"].isArray()) return false;
    record.tagRevision = state["revision"].toString().toULongLong();
    record.tagEndpoint = state["endpoint"].toString();
    const auto remotes = state["remotes"].toObject();
    for (auto it = remotes.begin(); it != remotes.end(); ++it)
    {
        if (!QRegularExpression("^[0-9a-f]{64}$").match(it.key()).hasMatch()) return false;
        const auto json = it.value().toObject();
        TagRemoteState remote;
        if (!readTagMap(json["base"], remote.base) || !readTagMap(json["pending"], remote.pending) || !readTagMap(json["conflicts"], remote.conflicts)) return false;
        remote.rebuildHead = json["rebuildHead"].toString();
        remote.rebuildExpected = json["rebuildExpected"].toString();
        remote.lastUploadedHead = json["lastUploadedHead"].toString();
        record.tagRemotes.insert(it.key(), remote);
    }
    for (const auto &value : state["journal"].toArray())
    {
        const auto json = value.toObject();
        if (!json["name"].isString() || !json["before"].isString() || !json["after"].isString()) return false;
        record.tagJournal.append({json["name"].toString(), json["before"].toString(), json["after"].toString()});
    }
    return true;
}
QJsonObject tagStateJson(const BackupRecord &record)
{
    QJsonObject remotes;
    for (auto it = record.tagRemotes.cbegin(); it != record.tagRemotes.cend(); ++it)
        remotes.insert(it.key(), QJsonObject{{"base", tagMapJson(it->base)}, {"pending", tagMapJson(it->pending)},
                       {"conflicts", tagMapJson(it->conflicts)}, {"rebuildHead", it->rebuildHead}, {"rebuildExpected", it->rebuildExpected}, {"lastUploadedHead", it->lastUploadedHead}});
    QJsonArray journal;
    for (const auto &change : record.tagJournal)
        journal.append(QJsonObject{{"name", change.name}, {"before", change.before}, {"after", change.after}});
    return {{"format", 1}, {"revision", QString::number(record.tagRevision)}, {"endpoint", record.tagEndpoint}, {"remotes", remotes}, {"journal", journal}};
}
}
TrackedItem BackupRecord::item() const
{
    return {id, sourcePath, QFileInfo(sourcePath).fileName(), state, stateDetail};
}
QString backupStateText(BackupSyncState state)
{
    switch (state)
    {
    case BackupSyncState::Tracking:
        return QStringLiteral("正常追踪");
    case BackupSyncState::RemotePending:
        return QStringLiteral("已拉取，源位置待处理");
    case BackupSyncState::ResolutionPending:
        return QStringLiteral("同步差异待处理");
    case BackupSyncState::NeedsAttention:
        return QStringLiteral("需要检查");
    }
    return {};
}
BackupCatalog::BackupCatalog(AppPaths paths) : m_paths(std::move(paths)) {}
bool BackupCatalog::validId(const QString &id)
{
    return !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id;
}
bool BackupCatalog::validRepositoryPath(const QString &path)
{
    if (path == ".")
        return true;
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.contains('\\') || path.contains(':') || path.contains(QChar(0)))
        return false;
    for (const auto &part : path.split('/'))
        if (part.isEmpty() || part == "." || part == ".." || part.compare(".git", Qt::CaseInsensitive) == 0)
            return false;
    return true;
}
QString BackupCatalog::normalizedSource(const QString &source)
{
    const QFileInfo info(source);
    const auto canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return QDir::cleanPath(canonical);
    const auto parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    return QDir::cleanPath(parent.isEmpty() ? info.absoluteFilePath() : QDir(parent).filePath(info.fileName()));
}
QString BackupCatalog::itemPath(const QString &id) const
{
    return validId(id) ? QDir(m_paths.backupRoot).filePath("items/" + id) : QString();
}
QString BackupCatalog::repoPath(const QString &id) const
{
    const auto p = itemPath(id);
    return p.isEmpty() ? QString() : p + "/repository";
}
QString BackupCatalog::stagingRoot() const { return QDir(m_paths.backupRoot).filePath("staging"); }
QString BackupCatalog::lockPath() const { return QDir(m_paths.backupRoot).filePath(".writer.lock"); }
const BackupRecord *BackupCatalog::find(const QString &id) const
{
    auto it = m_records.constFind(id);
    return it == m_records.cend() ? nullptr : &it.value();
}
QString BackupCatalog::idForSource(const QString &source) const
{
    const auto normalized = normalizedSource(source);
    for (const auto &record : m_records)
        if (QFileInfo(record.sourcePath) == QFileInfo(normalized))
            return record.id;
    return {};
}
OperationResult BackupCatalog::load()
{
    QMap<QString, BackupRecord> records;
    QStringList invalid;
    const QDir items(QDir(m_paths.backupRoot).filePath("items"));
    for (const auto &entry : items.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
    {
        if (!validId(entry.fileName()))
            continue;
        QFile file(entry.filePath() + "/record.json");
        if (!file.open(QIODevice::ReadOnly))
        {
            invalid.append(entry.filePath());
            continue;
        }
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
        const auto obj = doc.object();
        BackupRecord r;
        r.id = obj["id"].toString();
        r.sourcePath = obj["sourcePath"].toString();
        r.repositoryPath = obj["repositoryPath"].toString();
        r.directory = obj["directory"].toBool();
        r.generation = obj["generation"].toString().toULongLong();
        const int state = obj["state"].toInt(-1);
        if (error.error != QJsonParseError::NoError || obj["format"].toInt() != 1 || r.id != entry.fileName() ||
            !QDir::isAbsolutePath(r.sourcePath) || !validRepositoryPath(r.repositoryPath) || !r.generation || state < 0 || state > 3)
        {
            invalid.append(entry.filePath());
            continue;
        }
        if (!readTagState(obj["tags"], r)) { invalid.append(entry.filePath()); continue; }
        r.state = static_cast<BackupSyncState>(state);
        r.stateDetail = obj["stateDetail"].toString();
        r.lastCommit = obj["lastCommit"].toString();
        r.pendingCommit = obj["pendingCommit"].toString();
        r.operation = obj["operation"].toString();
        r.resolutionSession = obj["resolutionSession"].toString();
        r.resolutionHead = obj["resolutionHead"].toString();
        if (!r.resolutionSession.isEmpty() && !validId(r.resolutionSession))
        { invalid.append(entry.filePath()); continue; }
        const auto fingerprint = obj["fingerprint"].toObject();
        for (auto it = fingerprint.begin(); it != fingerprint.end(); ++it)
            r.fingerprint.insert(it.key(), it.value().toString());
        for (const auto &path : obj["recoveryPaths"].toArray())
            r.recoveryPaths.append(path.toString());
        if (!r.operation.isEmpty())
        {
            r.state = BackupSyncState::NeedsAttention;
            r.stateDetail = QStringLiteral("上次操作未完成（%1），自动备份已暂停。保留副本：%2").arg(r.operation, r.recoveryPaths.join("、"));
        }
        records.insert(r.id, r);
    }
    m_records = std::move(records);
    auto result = OperationResult::ok({});
    if (!invalid.isEmpty())
        result.warning = "以下追踪记录无法读取，原数据已保留：\n" + invalid.join('\n');
    return result;
}
OperationResult BackupCatalog::save(const BackupRecord &r)
{
    if (!validId(r.id) || !validRepositoryPath(r.repositoryPath) || !QDir::isAbsolutePath(r.sourcePath))
        return OperationResult::fail("保存失败", "追踪记录无效");
    if (allowSave && !allowSave(r))
        return OperationResult::fail("保存失败", "无法保存追踪状态");
    QJsonObject fingerprint;
    for (auto it = r.fingerprint.cbegin(); it != r.fingerprint.cend(); ++it)
        fingerprint.insert(it.key(), it.value());
    QJsonObject obj{{"format", 1}, {"id", r.id}, {"sourcePath", r.sourcePath}, {"repositoryPath", r.repositoryPath}, {"directory", r.directory}, {"generation", QString::number(r.generation)}, {"state", int(r.state)}, {"stateDetail", r.stateDetail}, {"lastCommit", r.lastCommit}, {"pendingCommit", r.pendingCommit}, {"operation", r.operation}, {"resolutionSession", r.resolutionSession}, {"resolutionHead", r.resolutionHead}, {"fingerprint", fingerprint}, {"recoveryPaths", QJsonArray::fromStringList(r.recoveryPaths)}};
    obj.insert("tags", tagStateJson(r));
    if (!QDir().mkpath(itemPath(r.id)))
        return OperationResult::fail("保存失败", "无法创建追踪记录目录");
    QSaveFile file(itemPath(r.id) + "/record.json");
    const auto data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        return OperationResult::fail("保存失败", "无法保存追踪状态：" + file.errorString());
    m_records.insert(r.id, r);
    return OperationResult::ok({});
}
OperationResult BackupCatalog::erase(const QString &id)
{
    if (!validId(id) || !QFile::remove(itemPath(id) + "/record.json"))
        return OperationResult::fail("删除失败", "无法移除追踪记录");
    m_records.remove(id);
    return OperationResult::ok({});
}
