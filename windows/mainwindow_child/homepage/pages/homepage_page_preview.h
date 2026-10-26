#pragma once
#include "utils/backupservice.h"
#include "utils/backupmonitor_directorywatcher.h"
#include "windows/mainwindow_navigation.h"
#include <QStandardItemModel>
#include <QTimer>
#include <QWidget>
#include <memory>
namespace Ui { class HomePagePreviewPage; }
class QAction;
class HomePagePreviewPage : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePagePreviewPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePagePreviewPage() override;
    void setBackup(const QString &id, const QString &path = {});
    void deactivate();
    void requestLeave(std::function<void()> continuation);
    bool hasUnsavedChanges() const;
    bool sourceOperationPending() const { return m_saving || m_mutating; }
    void refreshTheme();
    QList<QAction *> toolbarActions() const;
    QString rememberedPath(const QString &id) const { return m_paths.value(id); }
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);
    void loadingChanged(bool loading);
  protected:
    void resizeEvent(QResizeEvent *event) override;
  private:
    std::unique_ptr<Ui::HomePagePreviewPage> ui;
    BackupService *m_service;
    BackupDirectoryWatcher m_watcher;
    QTimer m_refreshTimer;
    QStandardItemModel m_model;
    QAction *m_refresh, *m_open, *m_new, *m_edit, *m_save, *m_cancel, *m_manage;
    bool m_editing{false}, m_newFile{false}, m_saving{false}, m_mutating{false}, m_prompt{false};
    QString m_statusText;
    void updateActions();
    void save(std::function<void()> continuation = {});
    void beginEdit();
    void discard();
    void createEntry(bool directory);
    void entryMenu(const SourceLocation &location, const QPoint &position);
    void mutateEntry(const SourceLocation &location, SourceMutationKind kind, const QString &name = {});
    void status(const QString &text);
    QString m_id, m_path, m_changesHead;
    QHash<QString, QString> m_paths;
    QHash<QString, QPoint> m_scrolls;
    QHash<QString, bool> m_rawModes;
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
