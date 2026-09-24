#pragma once
#include <QLocalServer>
#include <QLockFile>
#include <QObject>

class InstanceController : public QObject
{
    Q_OBJECT
  public:
    explicit InstanceController(const QString &root, QObject *parent = nullptr);
    bool acquire();
    bool forward(const QStringList &paths, QString *error);
    void stopAccepting();
  signals:
    void pathsReceived(const QStringList &paths);
    void activateRequested();

  private:
    QString m_name;
    QLockFile m_lock;
    QLocalServer m_server;
};
