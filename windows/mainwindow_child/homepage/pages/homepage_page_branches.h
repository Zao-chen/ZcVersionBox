#pragma once
#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QStandardItemModel>
#include <QWidget>
#include <memory>

namespace Ui { class HomePageBranchesPage; }
class QAction;
class HomePageBranchesPage : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePageBranchesPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePageBranchesPage() override;
    void setBackup(const QString &id);
    void refresh();
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
    QStandardItemModel m_branches, m_graph;
    QVector<Revision> m_revisions;
    QStringList m_graphTips;
    QAction *m_create, *m_refresh, *m_fetch;
    quint64 m_generation{0}, m_graphRequest{0};
    bool m_graphLoading{false};
    bool m_busy{false}, m_more{false};
    BackupTaskId m_task{0};
    BranchInfo selected() const;
    BranchRequest request(const BranchInfo &branch) const;
    void updateActions();
    void complete(const OperationResult &result);
    void showOperations();
    void loadGraph(bool append = false);
    void populateGraph();
    void compare(const BranchInfo &branch);
    void merge(const BranchInfo &branch);
};
