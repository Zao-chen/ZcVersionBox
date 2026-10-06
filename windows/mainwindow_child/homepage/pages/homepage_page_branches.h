#pragma once
#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QStandardItemModel>
#include <QWidget>
#include <QPointer>
#include <memory>

namespace Ui { class HomePageBranchesPage; }
class QAction;
class QDialog;
class HomePageBranchesPage : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePageBranchesPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePageBranchesPage() override;
    void setBackup(const QString &id);
    void refresh();
    void openManager();
    void requestSwitch(const QString &id, const QString &ref);
    void createFrom(const QString &id, const QString &commit = {});
    QList<QAction *> toolbarActions() const;
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);
  private:
    std::unique_ptr<Ui::HomePageBranchesPage> ui;
    BackupService *m_service;
    QString m_id;
    BranchSnapshot m_snapshot;
    QStandardItemModel m_branches;
    QPointer<QWidget> m_owner;
    QPointer<QDialog> m_dialog;
    QPointer<HomePageBranchesPage> m_dialogPage;
    QAction *m_create, *m_refresh, *m_fetch, *m_manage;
    quint64 m_generation{0};
    bool m_busy{false};
    BackupTaskId m_task{0};
    BranchInfo selected() const;
    BranchRequest request(const BranchInfo &branch) const;
    void updateActions();
    void complete(const OperationResult &result);
    void showOperations();
    QWidget *dialogOwner() const;
    void compare(const BranchInfo &branch);
    void merge(const BranchInfo &branch);
};
