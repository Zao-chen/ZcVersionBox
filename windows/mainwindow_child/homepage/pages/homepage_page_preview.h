#pragma once
#include "utils/backupservice.h"
#include "utils/backupmonitor_directorywatcher.h"
#include "windows/mainwindow_navigation.h"
#include <QStandardItemModel>
#include <QTimer>
#include <QWidget>
#include <memory>
namespace Ui { class HomePagePreviewPage; }
namespace oclero::qlementine { class LoadingSpinner; }
class QAction;
class HomePagePreviewPage : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePagePreviewPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePagePreviewPage() override;
    void setBackup(const QString &id, const QString &path = {});
    void deactivate();
    void refreshTheme();
    QList<QAction *> toolbarActions() const;
    QString rememberedPath(const QString &id) const { return m_paths.value(id); }
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);
  protected:
    void resizeEvent(QResizeEvent *event) override;
  private:
    std::unique_ptr<Ui::HomePagePreviewPage> ui;
    BackupService *m_service;
    BackupDirectoryWatcher m_watcher;
    QTimer m_refreshTimer;
    QStandardItemModel m_model;
    oclero::qlementine::LoadingSpinner *m_spinner;
    QAction *m_refresh, *m_open;
    QString m_id, m_path;
    QHash<QString, QString> m_paths;
    QHash<QString, QPoint> m_scrolls;
    SourceDirectory m_directory;
    SourceContent m_content;
    QMap<QString, Revision> m_changes;
    QVector<BackupTaskId> m_tasks;
    quint64 m_request{0}, m_changesRequest{0};
    bool m_active{false}, m_loading{false}, m_valid{false}, m_fit{true};
    double m_zoom{1.0};
    void refresh();
    void loadChanges(int offset = 0);
    void updateChanges();
    void breadcrumbs();
    void openPath(const QString &path);
    void showContent();
    void showImage();
    void setLoading(bool loading);
    void rememberScroll();
    void restoreScroll();
    void find(bool previous);
};
