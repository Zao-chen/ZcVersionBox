#pragma once
#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QWidget>
#include <memory>
namespace Ui
{
class HomePageDashboardPage;
}
namespace oclero::qlementine
{
class Switch;
class Expander;
} // namespace oclero::qlementine
class QAction;
class HomePageDashboardPage : public QWidget
{
    Q_OBJECT
  public:
    HomePageDashboardPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePageDashboardPage();
    void setBackup(const QString &id);
    void refresh();
    void deactivate();
    QList<QAction *> toolbarActions() const;
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    struct ViewState
    {
        quint64 generation{0};
        int scroll{0};
        bool expanded{false};
    };
    std::unique_ptr<Ui::HomePageDashboardPage> ui;
    BackupService *m_service;
    QString m_id;
    quint64 m_repositoryGeneration{0};
    QHash<QString, ViewState> m_states;
    oclero::qlementine::Switch *m_remoteSwitch;
    oclero::qlementine::Expander *m_expander;
    QAction *m_refresh;
    void remoteToggled(bool checked);
    void rememberState();
};
