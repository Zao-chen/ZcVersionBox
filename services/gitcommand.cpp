#include "gitcommand.h"
#include <QProcess>

GitResult runGit(const QString &repository, const QStringList &arguments)
{
    QProcess process;
    if (!repository.isEmpty())
        process.setWorkingDirectory(repository);
    process.start("git", arguments);
    GitResult result;
    result.started = process.waitForStarted();
    if (!result.started)
    {
        result.error = QStringLiteral("无法启动 git，请确认 git 已安装");
        return result;
    }
    result.finished = process.waitForFinished();
    result.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    result.output = QString::fromUtf8(process.readAllStandardOutput());
    result.error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (!result.finished)
    {
        process.kill();
        process.waitForFinished();
        result.error = QStringLiteral("Git 命令执行超时");
    }
    else if (result.exitCode != 0 && result.error.isEmpty())
        result.error = QStringLiteral("Git 命令执行失败（%1）").arg(result.exitCode);
    return result;
}
