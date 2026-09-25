#pragma once

#include "backup_types.h"
#include <atomic>
#include <memory>

struct BackupSelectionPolicy
{
    static bool observes(const QString &absolutePath);
    static bool copies(const QString &entryName);
};

// Observation and capture deliberately differ: build does not trigger a backup,
// but remains copyable. Recursive walks never follow links or directory junctions.
class BackupFiles
{
  public:
    virtual ~BackupFiles() = default;
    std::shared_ptr<std::atomic_bool> cancellation;
    virtual BackupResult<QStringList> children(const QString &path) const;
    virtual OperationResult copy(const QString &source, const QString &target) const;
    virtual OperationResult rename(const QString &source, const QString &target) const;
    virtual OperationResult remove(const QString &path) const;
    OperationResult fingerprint(const QString &path, bool directory, SourceFingerprint &result, bool observation = true) const;
    OperationResult capture(const QString &source, bool directory, const QString &target, SourceFingerprint &observed) const;
    OperationResult readOnly(const QString &path) const;
    static bool isLink(const QString &path);
    static bool hasLinkedAncestor(const QString &path);
    static bool isGitMetadataPath(const QString &path);
    static bool overlaps(const QString &first, const QString &second);
    static bool exists(const QString &path);
};

// An in-memory undo list for same-volume renames, with no persistent replay protocol.
class BackupReplacement
{
  public:
    BackupReplacement(std::shared_ptr<BackupFiles> files, QString target, QString stagingParent = {});
    ~BackupReplacement();
    QString stagingPath() const { return m_root + "/new"; }
    QString recoveryPath() const { return m_root; }
    bool valid() const { return !m_root.isEmpty(); }
    OperationResult install();
    OperationResult rollback();
    OperationResult verifyInstalled() const;
    OperationResult finish();
    void preserve() { m_preserve = true; }

  private:
    struct Move
    {
        QString from, to;
        SourceFingerprint contents;
    };
    std::shared_ptr<BackupFiles> m_files;
    QString m_target, m_root;
    QVector<Move> m_moves;
    SourceFingerprint m_before, m_expected;
    qsizetype m_applied{0};
    bool m_preserve{false}, m_started{false};
    OperationResult plan(const QString &candidate, const QString &target, const QString &relative);
    OperationResult planMove(const QString &source, const QString &target);
    OperationResult snapshot(const QString &path, SourceFingerprint &contents) const;
    OperationResult matches(const QString &path, const SourceFingerprint &expected) const;
    OperationResult verifyMoves() const;
    BackupResult<bool> hasGitMetadata(const QString &path) const;
};
