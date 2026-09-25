#pragma once
#include "services/backupservice.h"
#include "services/settingsservice.h"
#include "ui/navigation.h"
#include <QStandardItemModel>
#include <QWidget>
#include <memory>
namespace Ui
{
class DiffPage;
}
class DiffPage : public QWidget
{
    Q_OBJECT
  public:
    DiffPage(BackupService *service, SettingsService *settings, AiGateway *gateway, QWidget *parent = nullptr);
    ~DiffPage();
    void setRevision(const QString &id, const QString &commit);
    void deactivate();
    void refreshTheme();
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);

  private:
    std::unique_ptr<Ui::DiffPage> ui;
    BackupService *m_service;
    SettingsService *m_settings;
    AiGateway *m_gateway;
    QStandardItemModel m_model;
    QString m_id;
    DiffData m_diff;
    QString m_text;
    quint64 m_generation{0};
    void loadFile();
    void analyze();
};
