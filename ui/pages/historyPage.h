#pragma once
#include "services/backupservice.h"
#include "ui/navigation.h"
#include <QStandardItemModel>
#include <QWidget>
#include <memory>
namespace Ui
{
class HistoryPage;
}
class HistoryPage : public QWidget
{
    Q_OBJECT
  public:
    HistoryPage(BackupService *service, QWidget *parent = nullptr);
    ~HistoryPage();
    void setBackup(const QString &id);
    void refresh();
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);

  private:
    std::unique_ptr<Ui::HistoryPage> ui;
    BackupService *m_service;
    QString m_id;
    QStandardItemModel m_model;
    bool m_loading{false};
    QString selectedCommit() const;
    void updateActions();
};
