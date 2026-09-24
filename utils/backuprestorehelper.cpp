#include "backuprestorehelper.h"

#include "fileutils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>

namespace
{
constexpr int kGitStartTimeoutMs = 5000;
constexpr int kGitCheckoutTimeoutMs = 120000;
constexpr int kGitCleanupTimeoutMs = 30000;

void setMessage(QString *targetMessage, const QString &message)
{
    if (targetMessage)
        *targetMessage = message;
}

void appendMessage(QString *targetMessage, const QString &message)
{
    if (!targetMessage || message.isEmpty())
        return;

    if (!targetMessage->isEmpty())
        targetMessage->append(QLatin1Char('\n'));
    targetMessage->append(message);
}

bool removePath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return true;

    if (info.isDir() && !info.isSymLink())
        return QDir(path).removeRecursively();

    return QFile::remove(path);
}

bool renamePath(const QString &sourcePath, const QString &targetPath)
{
    const QFileInfo sourceInfo(sourcePath);
    if (sourceInfo.isDir() && !sourceInfo.isSymLink())
    {
        QDir parentDir(sourceInfo.absolutePath());
        return parentDir.rename(sourceInfo.fileName(), QFileInfo(targetPath).fileName());
    }

    return QFile::rename(sourcePath, targetPath);
}

bool copyPath(const QString &sourcePath, const QString &targetPath)
{
    const QFileInfo sourceInfo(sourcePath);
    if (sourceInfo.isDir() && !sourceInfo.isSymLink())
        return FileUtils::copyDirectory(sourcePath, targetPath);

    return QFile::copy(sourcePath, targetPath);
}

QString siblingTemporaryPath(const QString &targetPath, const QString &purpose)
{
    const QFileInfo targetInfo(targetPath);
    const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return QDir(targetInfo.absolutePath()).filePath(QStringLiteral(".%1.zcversionbox-%2-%3").arg(targetInfo.fileName(), purpose, suffix));
}

bool replacePathSafely(const QString &snapshotPath,
                       const QString &targetPath,
                       QString *errorMessage,
                       QString *warningMessage)
{
    const QFileInfo snapshotInfo(snapshotPath);
    if (!snapshotInfo.exists() && !snapshotInfo.isSymLink())
    {
        setMessage(errorMessage, QStringLiteral("所选版本中不存在要恢复的内容"));
        return false;
    }

    const QFileInfo targetInfo(targetPath);
    if (targetInfo.fileName().isEmpty())
    {
        setMessage(errorMessage, QStringLiteral("目标路径无效"));
        return false;
    }

    QDir targetParent(targetInfo.absolutePath());
    if (!targetParent.exists() && !QDir().mkpath(targetParent.absolutePath()))
    {
        setMessage(errorMessage, QStringLiteral("无法创建目标目录，请检查权限"));
        return false;
    }

    const QString stagingPath = siblingTemporaryPath(targetPath, QStringLiteral("staging"));
    const QString rollbackPath = siblingTemporaryPath(targetPath, QStringLiteral("rollback"));
    if (!copyPath(snapshotPath, stagingPath))
    {
        removePath(stagingPath);
        setMessage(errorMessage, QStringLiteral("无法准备恢复内容，请检查磁盘空间和权限"));
        return false;
    }

    const bool targetExists = targetInfo.exists() || targetInfo.isSymLink();
    if (targetExists && !renamePath(targetPath, rollbackPath))
    {
        removePath(stagingPath);
        setMessage(errorMessage, QStringLiteral("无法暂存当前内容，请检查文件占用状态"));
        return false;
    }

    if (!renamePath(stagingPath, targetPath))
    {
        removePath(stagingPath);
        if (targetExists && !renamePath(rollbackPath, targetPath))
        {
            setMessage(errorMessage,
                       QStringLiteral("替换失败且无法自动回滚；原内容保留在：%1").arg(rollbackPath));
            return false;
        }

        setMessage(errorMessage, QStringLiteral("替换恢复内容失败，原内容已自动还原"));
        return false;
    }

    if (targetExists && !removePath(rollbackPath))
    {
        const QString warning = QStringLiteral("恢复已完成，但无法清理原内容副本：%1。请确认恢复结果后手动删除。")
                                    .arg(rollbackPath);
        appendMessage(warningMessage, warning);
        qWarning() << warning;
    }

    return true;
}

QString processError(QProcess &process, const QString &fallback)
{
    const QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return detail.isEmpty() ? fallback : detail;
}

bool runGitCommand(const QString &repoPath,
                   const QStringList &arguments,
                   int finishTimeoutMs,
                   QString *errorMessage,
                   QString *standardOutput = nullptr)
{
    setMessage(errorMessage, QString());
    setMessage(standardOutput, QString());

    QProcess process;
    process.setWorkingDirectory(repoPath);
    process.start(QStringLiteral("git"), arguments);
    if (!process.waitForStarted(kGitStartTimeoutMs))
    {
        setMessage(errorMessage, QStringLiteral("无法启动 git，请确认 git 已安装"));
        return false;
    }

    if (!process.waitForFinished(finishTimeoutMs))
    {
        process.kill();
        process.waitForFinished(kGitStartTimeoutMs);
        setMessage(errorMessage,
                   QStringLiteral("Git 操作超过 %1 秒，已终止以避免使用不完整数据")
                       .arg(finishTimeoutMs / 1000));
        return false;
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        setMessage(errorMessage, processError(process, QStringLiteral("Git 操作失败")));
        return false;
    }

    setMessage(standardOutput, QString::fromUtf8(process.readAllStandardOutput()));
    return true;
}

bool isWorktreeRegistered(const QString &repoPath,
                          const QString &snapshotRoot,
                          bool *registered,
                          QString *errorMessage)
{
    QString output;
    if (!runGitCommand(repoPath,
                       QStringList() << QStringLiteral("worktree") << QStringLiteral("list")
                                     << QStringLiteral("--porcelain"),
                       kGitCleanupTimeoutMs,
                       errorMessage,
                       &output))
    {
        return false;
    }

    const QString expectedPath = QDir::cleanPath(QFileInfo(snapshotRoot).absoluteFilePath());
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity pathCaseSensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity pathCaseSensitivity = Qt::CaseSensitive;
#endif

    *registered = false;
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines)
    {
        if (!line.startsWith(QStringLiteral("worktree ")))
            continue;

        const QString listedPath = QDir::cleanPath(QFileInfo(line.mid(9).trimmed()).absoluteFilePath());
        if (listedPath.compare(expectedPath, pathCaseSensitivity) == 0)
        {
            *registered = true;
            break;
        }
    }
    return true;
}

bool cleanupTemporaryWorktree(const QString &repoPath,
                              const QString &snapshotRoot,
                              QString *warningMessage)
{
    QString removeError;
    if (runGitCommand(repoPath,
                      QStringList() << QStringLiteral("worktree") << QStringLiteral("remove")
                                    << QStringLiteral("--force") << snapshotRoot,
                      kGitCleanupTimeoutMs,
                      &removeError))
    {
        return true;
    }

    const bool directoryRemoved = !QFileInfo::exists(snapshotRoot) || QDir(snapshotRoot).removeRecursively();
    bool registered = false;
    QString listError;
    if (isWorktreeRegistered(repoPath, snapshotRoot, &registered, &listError) && !registered && directoryRemoved)
        return true;

    QString retryError;
    if (registered)
    {
        runGitCommand(repoPath,
                      QStringList() << QStringLiteral("worktree") << QStringLiteral("remove")
                                    << QStringLiteral("--force") << snapshotRoot,
                      kGitCleanupTimeoutMs,
                      &retryError);
        if (isWorktreeRegistered(repoPath, snapshotRoot, &registered, &listError) && !registered && directoryRemoved)
            return true;
    }

    const QString warning = QStringLiteral("无法完整清理临时恢复目录或 Git worktree 元数据：%1").arg(snapshotRoot);
    appendMessage(warningMessage, warning);
    qWarning() << warning << removeError << retryError << listError;
    return false;
}
} // namespace

BackupRestoreResult BackupRestoreHelper::restoreFromGitRevision(const QString &repoPath,
                                                                const QString &revision,
                                                                const QString &targetPath)
{
    BackupRestoreResult result;
    if (repoPath.isEmpty() || revision.isEmpty() || targetPath.isEmpty())
    {
        result.errorMessage = QStringLiteral("恢复参数不完整");
        return result;
    }

    const QFileInfo targetInfo(QDir::cleanPath(targetPath));
    if (targetInfo.fileName().isEmpty())
    {
        result.errorMessage = QStringLiteral("无法确定要恢复的文件名");
        return result;
    }

    QTemporaryDir temporaryRoot(QDir::tempPath() + QStringLiteral("/ZcVersionBoxRestore-XXXXXX"));
    if (!temporaryRoot.isValid())
    {
        result.errorMessage = QStringLiteral("无法创建恢复临时目录");
        return result;
    }

    const QString snapshotRoot = QDir(temporaryRoot.path()).filePath(QStringLiteral("snapshot"));
    QString gitError;
    if (!runGitCommand(repoPath,
                       QStringList() << QStringLiteral("worktree") << QStringLiteral("add")
                                     << QStringLiteral("--detach") << snapshotRoot << revision,
                       kGitCheckoutTimeoutMs,
                       &gitError))
    {
        QString cleanupWarning;
        cleanupTemporaryWorktree(repoPath, snapshotRoot, &cleanupWarning);
        appendMessage(&gitError, cleanupWarning);
        result.errorMessage = gitError.isEmpty() ? QStringLiteral("无法读取所选历史版本") : gitError;
        return result;
    }

    const QString snapshotPath = QDir(snapshotRoot).filePath(targetInfo.fileName());
    result.success = replacePathSafely(snapshotPath,
                                       targetInfo.absoluteFilePath(),
                                       &result.errorMessage,
                                       &result.warningMessage);
    QString cleanupWarning;
    cleanupTemporaryWorktree(repoPath, snapshotRoot, &cleanupWarning);
    if (result.success)
        appendMessage(&result.warningMessage, cleanupWarning);
    else
        appendMessage(&result.errorMessage, cleanupWarning);
    return result;
}
