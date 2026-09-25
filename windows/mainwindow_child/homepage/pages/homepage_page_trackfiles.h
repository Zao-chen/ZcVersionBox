#pragma once
#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QObject>
class QAction;
class QMenu;
class QWidget;

// UI-owned actions and dialogs. Services, routes and the backup lifecycle do
// not depend on a menu, a page instance, or any Qlementine control.
class BackupUiActions : public QObject
{
    Q_OBJECT
  public:
    BackupUiActions(BackupService *service, QWidget *owner);
    QAction *addAction() const { return m_add; }
    QMenu *addMenu() const { return m_addMenu; }
    void setCurrentBackup(const QString &id) { m_currentId = id; }
    void showObjectMenu(const QString &id, const QPoint &position, bool maintenance = true);
    void remove(const QString &id);
    void rebuild(const QString &id);
    void refreshIcons();
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);

  private:
    BackupService *m_service;
    QWidget *m_owner;
    QAction *m_add;
    QMenu *m_addMenu;
    QAction *m_open;
    QAction *m_overview;
    QAction *m_copy;
    QAction *m_remove;
    QAction *m_rebuild;
    QString m_currentId;
    QString m_menuId;
    QString target() const { return m_menuId.isEmpty() ? m_currentId : m_menuId; }
    void addLocal(bool directory);
    void importRemote();
};
