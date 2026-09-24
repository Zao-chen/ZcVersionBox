#pragma once
#include "backup/backupservice.h"
#include <QWidget>
#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTextEdit;

class HomePage : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePage(Backup::BackupService *service, QWidget *parent = nullptr);

  private:
    Backup::BackupService *m_service;
    QListWidget *m_objects;
    QLabel *m_title, *m_source, *m_status;
    QLineEdit *m_remote;
    QTableWidget *m_history;
    QTabWidget *m_tabs;
    QTextEdit *m_diff;
    QPushButton *m_more;
    QString m_selected;
    int m_historyGeneration = 0;
    QMap<QString, QMap<QString, QString>> m_states;
    QMap<QString, std::function<void(const Backup::TaskResult &)>> m_callbacks;
    void refreshObjects();
    void selectObject();
    void renderState();
    void loadHistory(bool append = false);
    QString selectedRevision() const;
    void send(Backup::Request request, std::function<void(const Backup::TaskResult &)> callback = {});
    void action(const QString &operation);
    void addLocal(bool directory);
    void importRemote();
    void notify(const QString &message, bool error = true);
};
