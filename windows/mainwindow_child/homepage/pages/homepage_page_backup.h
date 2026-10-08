#pragma once
#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QPointer>
#include <QStandardItemModel>
#include <QWidget>
#include <memory>
namespace Ui
{
class HomePageBackupPage;
}
class QAction;
class QMenu;
class QDialog;
class QAbstractButton;
namespace UiDialog
{
class Dialog;
}
class HomePageBackupPage : public QWidget
{
    Q_OBJECT
  public:
    HomePageBackupPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePageBackupPage();
    void setBackup(const QString &id, const QString &commit = {}, const QString &branchRef = {});
    void setBranchDetail(const QString &id, const QString &branchRef, bool includeCommonAncestors = false);
    void refresh();
    void deactivate();
    QList<QAction *> toolbarActions() const;
  signals:
    void navigate(const Route &route);
    void createBranchRequested(const QString &id, const QString &commit);
    void branchRequested(const QString &branchRef);
    void commonAncestorsExpansionChanged(bool expanded);
    void notification(const OperationResult &result);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;

  private:
    struct ViewState
    {
        quint64 generation{0};
        QString commit;
        int scroll{0};
        bool milestoneOnly{false};
    };
    struct RevisionContext
    {
        QString backupId;
        QString commit;
        quint64 repositoryGeneration{0};
        quint64 pageGeneration{0};
        BranchContext workingBranch;
    };
    std::unique_ptr<Ui::HomePageBackupPage> ui;
    BackupService *m_service;
    QString m_id;
    QString m_branchRef;
    bool m_branchDetail{false};
    bool m_includeCommonAncestors{false};
    int m_commonAncestorCount{0};
    int m_uniqueRevisionCount{0};
    QString m_loadedId;
    QString m_requestedCommit;
    quint64 m_loadedGeneration{0};
    BranchContext m_loadedBranch;
    QHash<QString, ViewState> m_states;
    QStandardItemModel m_model;
    QAction *m_compare;
    QAction *m_preview;
    QAction *m_restore;
    QAction *m_edit;
    QAction *m_more;
    QAction *m_refresh;
    QAction *m_tag;
    QAbstractButton *m_milestoneOnly;
    QPointer<QDialog> m_tagDialog;
    QPointer<QMenu> m_revisionMenu;
    quint64 m_contextGeneration{0};
    bool m_loading{false};
    bool m_editing{false};
    bool m_refreshPending{false};
    bool m_refreshNeeded{false};
    quint64 m_refreshGeneration{0};
    QString selectedCommit() const;
    void updateActions();
    void toggleCommonAncestors();
    void rememberState();
    void applyFilter();
    void manageTags(const RevisionContext &context);
    void resolveTagConflicts();
    QVector<VersionTag> revisionTags(const RevisionContext &context) const;
    void trackTagDialog(UiDialog::Dialog *dialog);
    RevisionContext revisionContext(const QModelIndex &index) const;
    bool isCurrentContext(const RevisionContext &context) const;
    QModelIndex indexForRevision(const RevisionContext &context) const;
    void compareRevision(const RevisionContext &context);
    void previewRevision(const RevisionContext &context);
    void restoreRevision(const RevisionContext &context);
    void editRevision(const RevisionContext &context);
    void showRevisionMenu(const RevisionContext &context, const QPoint &position);
    void showBranchMenu(const RevisionContext &context, const QStringList &refs, const QPoint &position);
    void closeRevisionMenu();
};
