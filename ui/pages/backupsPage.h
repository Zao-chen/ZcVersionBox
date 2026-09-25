#pragma once
#include "services/backupservice.h"
#include "ui/navigation.h"
#include <QWidget>
#include <memory>
namespace Ui
{
class BackupsPage;
}
class BackupsPage : public QWidget
{
    Q_OBJECT
  public:
    BackupsPage(BackupService *service, QWidget *parent = nullptr);
    ~BackupsPage();
    void refresh();
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);

  private:
    std::unique_ptr<Ui::BackupsPage> ui;
    BackupService *m_service;
    void addLocal();
    void importRemote();
};
