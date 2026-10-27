#pragma once

#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QPointer>
#include <QWidget>
#include <memory>

namespace Ui { class HomePageBranchesPage; }
class QAction;
class HomePageBranchRow;

class HomePageBranchesPage : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePageBranchesPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePageBranchesPage() override;
    void setBackup(const QString &id);
    void refresh();
    void requestSwitch(const QString &id, const QString &ref, std::function<void()> continuation = {});
    void createFrom(const QString &id, const QString &commit = {});
    QList<QAction *> toolbarActions() const;
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);
    void loadingChanged(bool loading);
  protected:
    void resizeEvent(QResizeEvent *event) override;
  private:
    std::unique_ptr<Ui::HomePageBranchesPage> ui;
    BackupService *m_service;
    QString m_id;
    BranchSnapshot m_snapshot;
    QAction *m_refresh{nullptr};
    bool m_loading{false};
    quint64 m_generation{0};
    bool m_busy{false};
    BackupTaskId m_task{0};
    QList<HomePageBranchRow *> m_rows;
    BranchRequest request(const BranchInfo &branch) const;
    QWidget *dialogOwner() const;
    void clearList(const QString &placeholder = {});
    void populateList();
    void setLoading(bool loading);
    void updateActions();
    void complete(const OperationResult &result, bool reload = true);
    void useBranch(const BranchInfo &branch);
    void showHistory(const BranchInfo &branch);
    void create(const QString &id, const QString &commit, const BranchInfo &tracked);
    void showOperations(const BranchInfo &selected, const QPoint &globalPos);
    void rename(const BranchInfo &selected);
    void remove(const BranchInfo &selected, bool force);
    void compare(const BranchInfo &selected, const BranchRequest &request, const QVector<BranchInfo> &branches);
    void merge(const BranchRequest &request);
};
