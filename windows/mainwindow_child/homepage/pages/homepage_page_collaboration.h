#pragma once
#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QWidget>
#include <memory>
namespace Ui { class HomePageCollaborationPage; }
namespace oclero::qlementine { class Switch; }
class HomePageCollaborationPage : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePageCollaborationPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePageCollaborationPage() override;
    void setBackup(const QString &id);
    void deactivate();
    void synchronize();
    void refresh();
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);
    void switchRequested(const QString &id, const QString &ref);
    void loadingChanged(bool loading);
  protected:
    void resizeEvent(QResizeEvent *event) override;
  private:
    std::unique_ptr<Ui::HomePageCollaborationPage> ui;
    BackupService *m_service;
    oclero::qlementine::Switch *m_automatic;
    QString m_id;
    quint64 m_generation{0}, m_context{0};
    bool m_active{false}, m_connecting{false}, m_loading{false};
    void setLoading(bool loading);
    QHash<QString, int> m_scrolls;
    bool current(const QString &id, quint64 generation, quint64 context) const;
    void connectRemote();
};
