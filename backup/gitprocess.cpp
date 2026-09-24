#include "gitprocess.h"
#include <QDir>
#include <QProcessEnvironment>

namespace Backup
{
GitProcess::GitProcess(const QString &repo, const QStringList &args, Cancellation cancel, int timeout)
    : m_cancel(std::move(cancel)), m_timeout(timeout)
{
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto &key : env.keys())
        if (key.startsWith("GIT_") && key != "GIT_SSH" && key != "GIT_SSH_COMMAND")
            env.remove(key);
    env.insert("GIT_TERMINAL_PROMPT", "0");
    env.insert("LC_ALL", "C");
    m_process.setProcessEnvironment(env);
    QStringList options{"-c", "commit.gpgsign=false", "-c", "core.quotepath=false"};
    if (!repo.isEmpty())
    {
        const auto hooks = QDir(repo).filePath("disabled-hooks");
        if (!QDir().mkpath(hooks))
            throw Error(ErrorCode::Permission, "无法创建仓库控制目录");
        options << "-c" << "core.hooksPath=" + hooks << "--git-dir=" + repo;
    }
    m_elapsed.start();
    m_process.start("git", options + args);
    if (!m_process.waitForStarted(5000))
        throw Error(ErrorCode::Git, "无法启动 Git：" + m_process.errorString());
}
GitProcess::~GitProcess()
{
    if (m_process.state() != QProcess::NotRunning)
    {
        m_process.kill();
        m_process.waitForFinished(5000);
    }
}
void GitProcess::drainError()
{
    m_error += m_process.readAllStandardError();
    if (m_error.size() > 65536)
        m_error = m_error.right(65536);
}
void GitProcess::tick()
{
    checkCancelled(m_cancel);
    drainError();
    if (m_elapsed.elapsed() > m_timeout)
        throw Error(ErrorCode::Timeout, "Git 操作超时", true);
}
void GitProcess::write(const QByteArray &bytes)
{
    tick();
    if (m_process.write(bytes) != bytes.size())
        throw Error(ErrorCode::Git, "Git 输入失败：" + QString::fromUtf8(m_error));
    while (m_process.bytesToWrite() > 1024 * 1024)
    {
        tick();
        m_process.waitForBytesWritten(50);
        if (m_process.state() == QProcess::NotRunning)
        {
            finish();
            throw Error(ErrorCode::Git, "Git 提前退出");
        }
    }
}
void GitProcess::closeInput() { m_process.closeWriteChannel(); }
QByteArray GitProcess::readLine()
{
    while (!m_process.canReadLine())
    {
        tick();
        if (m_process.state() == QProcess::NotRunning)
            throw Error(ErrorCode::Git, "Git 输出不完整：" + QString::fromUtf8(m_error));
        m_process.waitForReadyRead(50);
    }
    return m_process.readLine();
}
QByteArray GitProcess::read(qint64 maximum)
{
    while (m_process.bytesAvailable() == 0)
    {
        tick();
        if (m_process.state() == QProcess::NotRunning)
            throw Error(ErrorCode::Git, "Git 对象读取中断");
        m_process.waitForReadyRead(50);
    }
    return m_process.read(maximum);
}
QByteArray GitProcess::finish(const QList<int> &accepted)
{
    QByteArray output;
    while (m_process.state() != QProcess::NotRunning)
    {
        tick();
        m_process.waitForFinished(50);
        output += m_process.readAllStandardOutput();
        if (output.size() > 64 * 1024 * 1024)
            throw Error(ErrorCode::Unsupported, "Git 输出超过限制，请缩小查询范围");
    }
    output += m_process.readAllStandardOutput();
    drainError();
    if (m_process.exitStatus() != QProcess::NormalExit || !accepted.contains(m_process.exitCode()))
    {
        const auto message = QString::fromUtf8(m_error).trimmed();
        const bool denied = message.contains("Permission denied", Qt::CaseInsensitive) || message.contains("Authentication failed", Qt::CaseInsensitive);
        const bool transient = message.contains("Could not resolve", Qt::CaseInsensitive) || message.contains("Connection", Qt::CaseInsensitive) || message.contains("timed out", Qt::CaseInsensitive);
        throw Error(denied ? ErrorCode::Permission : ErrorCode::Git, message.isEmpty() ? "Git 操作失败" : message, transient);
    }
    return output;
}
QByteArray git(const QString &repo, const QStringList &args, const QByteArray &input, Cancellation cancel, const QList<int> &accepted, int timeout)
{
    GitProcess p(repo, args, std::move(cancel), timeout);
    p.write(input);
    p.closeInput();
    return p.finish(accepted);
}
} // namespace Backup
