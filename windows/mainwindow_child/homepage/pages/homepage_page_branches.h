#pragma once

#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QPointer>
#include <QWidget>
#include <memory>

namespace Ui { class HomePageBranchesPage; }
class QAction;
class HomePageBackupPage;

class HomePageBranchesPage : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePageBranchesPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePageBranchesPage() override;
    void setBackup(const QString &id, const QString &branchRef);
    void refresh();
    void requestSwitch(const QString &id, const QString &ref);
    void createFrom(const QString &id, const QString &commit = {});
    QList<QAction *> toolbarActions() const;
  signals:
    void navigate(const Route &route);
    void replaceRoute(const Route &route);
    void notification(const OperationResult &result);
  private:
    std::unique_ptr<Ui::HomePageBranchesPage> ui;
    BackupService *m_service;
    HomePageBackupPage *m_history{nullptr};
    QString m_id, m_branchRef;
    BranchSnapshot m_snapshot;
    BranchInfo m_branch;
    QAction *m_create{nullptr}, *m_refresh{nullptr};
    quint64 m_generation{0};
    bool m_busy{false};
    BackupTaskId m_task{0};
    bool m_historyExpanded{false};
    BranchRequest request() const;
    QWidget *dialogOwner() const;
    void updateActions();
    void complete(const OperationResult &result, bool reload = true);
    void showOperations();
    void compare(const BranchInfo &selected, const BranchRequest &request, const QVector<BranchInfo> &branches);
    void merge(const BranchRequest &request);
};
