#pragma once
#include <QObject>
#include <QStringList>

namespace Backup
{
class SourceMonitor : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
    virtual void start() = 0;
    virtual void stop() = 0;
    static SourceMonitor *create(const QString &source, QObject *parent);
  signals:
    void ready();
    void changed(const QStringList &paths, bool eventsLost);
    void failed(const QString &message);
};
} // namespace Backup
