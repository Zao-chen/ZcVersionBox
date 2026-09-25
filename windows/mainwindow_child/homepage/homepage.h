#pragma once
#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QWidget>
#include <memory>
namespace Ui
{
class HomePage;
}
class QAction;
class BackupListModel;
class BackupFilterModel;
class BackupUiActions;
class HomePage : public QWidget
{
    Q_OBJECT
  public:
    HomePage(BackupService *service, QWidget *parent = nullptr, BackupListModel *model = nullptr, BackupUiActions *actions = nullptr);
    ~HomePage();
    void refresh();
    QList<QAction *> toolbarActions() const;
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    std::unique_ptr<Ui::HomePage> ui;
    BackupService *m_service;
    BackupListModel *m_model;
    BackupFilterModel *m_filter;
    BackupUiActions *m_actions;
    bool m_ownsModel;
    void updateEmptyState();
};
