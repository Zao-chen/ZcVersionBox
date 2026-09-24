#pragma once
#include "types.h"
#include <QElapsedTimer>
#include <QProcess>

namespace Backup
{
class GitProcess
{
  public:
    GitProcess(const QString &repo, const QStringList &arguments, Cancellation cancel = {}, int timeout = 120000);
    ~GitProcess();
    void write(const QByteArray &bytes);
    void closeInput();
    QByteArray readLine();
    QByteArray read(qint64 maximum);
    QByteArray finish(const QList<int> &accepted = {0});
    int exitCode() const { return m_process.exitCode(); }

  private:
    void tick();
    void drainError();
    QProcess m_process;
    Cancellation m_cancel;
    QElapsedTimer m_elapsed;
    int m_timeout;
    QByteArray m_error;
};
QByteArray git(const QString &repo, const QStringList &args, const QByteArray &input = {}, Cancellation cancel = {}, const QList<int> &accepted = {0}, int timeout = 120000);
} // namespace Backup
