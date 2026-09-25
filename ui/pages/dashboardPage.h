#pragma once
#include "services/backupservice.h"
#include "ui/navigation.h"
#include <QWidget>
#include <memory>
namespace Ui
{
class DashboardPage;
}
namespace oclero::qlementine
{
class Switch;
class Expander;
} // namespace oclero::qlementine
class DashboardPage : public QWidget
{
    Q_OBJECT
  public:
    DashboardPage(BackupService *service, QWidget *parent = nullptr);
    ~DashboardPage();
    void setBackup(const QString &id);
    void refresh();
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);

  private:
    std::unique_ptr<Ui::DashboardPage> ui;
    BackupService *m_service;
    QString m_id;
    oclero::qlementine::Switch *m_remoteSwitch;
    oclero::qlementine::Expander *m_expander;
    void remoteToggled(bool checked);
};
