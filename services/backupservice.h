#pragma once

#include "apppaths.h"
#include "operationresult.h"
#include <QHash>
#include <QObject>
#include <QVector>
#include <functional>

struct TrackedItem
{
    QString id;
    QString sourcePath;
    QString name;
};
struct BackupStats
{
    int versionCount{0};
    int fileCount{0};
    qint64 fileSize{0};
    qint64 cacheSize{0};
    QString sourceState;
    QString remoteUrl;
};
struct Revision
{
    QString hash;
    QString message;
};
struct DiffFile
{
    QString status;
    QString path;
    QString summary;
};
struct DiffData
{
    QString oldCommit;
    QString newCommit;
    QVector<DiffFile> files;
};
struct PreparedImport
{
    OperationResult result;
    QString temporaryRepo;
    QString entryName;
    bool directory{false};
};

class BackupService : public QObject
{
    Q_OBJECT
  public:
    using CommitMessageGenerator = std::function<QString(const QString &diff, const QString &settingsFile)>;
    explicit BackupService(const AppPaths &paths, QObject *parent = nullptr, CommitMessageGenerator generator = {});
    const AppPaths &paths() const { return m_paths; }
    QVector<TrackedItem> trackedItems() const;
    QString sourcePath(const QString &id) const;
    QString repoPath(const QString &id) const;
    bool contains(const QString &id) const;
    quint64 repositoryGeneration(const QString &id) const { return m_generations.value(id); }
    OperationResult addLocal(const QString &source);
    virtual OperationResult backup(const QString &id);
    OperationResult statistics(const QString &id, BackupStats &stats) const;
    OperationResult history(const QString &id, QVector<Revision> &revisions) const;
    OperationResult diff(const QString &id, const QString &commit, DiffData &data) const;
    OperationResult diffText(const QString &id, const DiffData &data, const QString &file, QString &text) const;
    OperationResult preview(const QString &id, const QString &commit);
    OperationResult restore(const QString &id, const QString &commit);
    OperationResult editMessage(const QString &id, const QString &commit, const QString &message);
    OperationResult setRemote(const QString &id, const QString &url);
    OperationResult removeRemote(const QString &id);
    OperationResult synchronize(const QString &id, bool push);
    OperationResult removeBackup(const QString &id);
    OperationResult rebuild(const QString &id);
    OperationResult checkRemote(const QString &url) const;
    PreparedImport prepareImport(const QString &url);
    OperationResult finishImport(const QString &temporaryRepo, const QString &target);
    void cancelImport(const QString &temporaryRepo);

  signals:
    void trackedItemsChanged();
    void repositoryChanged(const QString &id);
    void repositoryInvalidated(const QString &id);

  private:
    AppPaths m_paths;
    CommitMessageGenerator m_generateMessage;
    QHash<QString, quint64> m_generations;
    bool ownsImport(const QString &path) const;
    QStringList m_imports;
};
