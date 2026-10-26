#pragma once
#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QWidget>
#include <memory>
namespace Ui
{
class HomePageDashboardPage;
}
class QAction;
class QLabel;
class QPushButton;
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
    void loadingChanged(bool loading);

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    struct ViewState
    {
        quint64 generation{0};
        int scroll{0};
    };
    std::unique_ptr<Ui::HomePageDashboardPage> ui;
    BackupService *m_service;
    QString m_id;
    quint64 m_repositoryGeneration{0};
    quint64 m_contextGeneration{0}, m_requestGeneration{0};
    bool m_active{false};
    QHash<QString, ViewState> m_states;
    QAction *m_refresh;
    QLabel *m_syncState, *m_syncDetail;
    QPushButton *m_continueResolution, *m_recheck;
    QWidget *m_syncPanel{nullptr};
    bool m_loading{false};
    QString m_loadedId;
    void setLoading(bool loading);
    void rememberState();
    void updateActions();
    bool isCurrent(const QString &id, quint64 generation, quint64 context) const;
    BackupService::Completion completion();
};
