#include "gitcommand.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>

GitResult runGit(const QString &repository, const QStringList &arguments, const GitOptions &options)
{
    QProcess process;
    if (!repository.isEmpty())
        process.setWorkingDirectory(repository);
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto *key : {"GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR", "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES"})
        environment.remove(key);
    environment.insert("GIT_TERMINAL_PROMPT", "0");
    environment.insert("GCM_INTERACTIVE", "Never");
    process.setProcessEnvironment(environment);
    GitResult result;
    if (options.cancel && options.cancel->load())
    {
        result.cancelled = true;
        result.error = "操作已取消";
        return result;
    }
    process.start("git", arguments);
    result.started = process.waitForStarted(5000);
    if (!result.started)
    {
        result.error = QStringLiteral("无法启动 git，请确认 git 已安装");
        return result;
    }
    if (!options.input.isEmpty())
        process.write(options.input);
    process.closeWriteChannel();
    QElapsedTimer timer;
    timer.start();
    QByteArray errors;
    while (process.state() != QProcess::NotRunning)
    {
        process.waitForFinished(50);
        result.bytes += process.readAllStandardOutput();
        errors += process.readAllStandardError();
        result.cancelled = options.cancel && options.cancel->load();
        if (result.cancelled || timer.elapsed() > options.timeoutMs)
        {
            process.kill();
            process.waitForFinished(5000);
            result.error = result.cancelled ? QStringLiteral("操作已取消") : QStringLiteral("Git 命令执行超时");
            break;
        }
    }
    result.bytes += process.readAllStandardOutput();
    errors += process.readAllStandardError();
    result.finished = result.error.isEmpty();
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.output = QString::fromUtf8(result.bytes);
    if (result.error.isEmpty())
        result.error = QString::fromUtf8(errors).trimmed();
    if (result.exitCode != 0 && result.error.isEmpty())
        result.error = QStringLiteral("Git 命令执行失败（%1）").arg(result.exitCode);
    return result;
}
