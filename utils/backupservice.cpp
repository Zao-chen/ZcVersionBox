#include "backupservice.h"
#include "gitcommand.h"
#include "utils/aicommitmessagehelper.h"
#include "utils/backuprestorehelper.h"
#include "utils/fileutils.h"
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QSettings>
#include <QTimeZone>
#include <QUrl>
#include <QUuid>

namespace
{
bool removePath(const QString &path)
{
    const QFileInfo info(path);
    return !info.exists() || (info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path));
}
bool copyPath(const QString &source, const QString &target)
{
    return QFileInfo(source).isDir() ? FileUtils::copyDirectory(source, target) : QFile::copy(source, target);
}
QPair<int, qint64> pathStats(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return {};
    if (info.isFile())
        return {1, info.size()};
    QPair<int, qint64> stats;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::Readable | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        ++stats.first;
        stats.second += it.fileInfo().size();
    }
    return stats;
}
QFileInfo importEntry(const QString &path)
{
    for (const auto &entry : QDir(path).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name))
        if (entry.fileName() != ".git")
            return entry;
    return {};
}
OperationResult missing() { return OperationResult::fail("操作失败", "本地备份仓库不存在"); }
} // namespace

BackupService::BackupService(const AppPaths &paths, QObject *parent, CommitMessageGenerator generator)
    : QObject(parent), m_paths(paths), m_generateMessage(std::move(generator))
{
    if (!m_generateMessage)
        m_generateMessage = [](const QString &diff, const QString &settingsFile)
        {
            return AiCommitMessageHelper::generateCommitMessageSync(diff, 15000, nullptr, settingsFile);
        };
}
QString BackupService::sourcePath(const QString &id) const { return QUrl::fromPercentEncoding(id.toUtf8()); }
QString BackupService::repoPath(const QString &id) const { return QDir(m_paths.backupRoot).filePath(id); }
bool BackupService::contains(const QString &id) const
{
    return !id.isEmpty() && id != "." && id != ".." && !id.contains('/') && !id.contains('\\') && QDir(repoPath(id)).exists();
}
QVector<TrackedItem> BackupService::trackedItems() const
{
    QVector<TrackedItem> items;
    for (const auto &id : QDir(m_paths.backupRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
        if (id.startsWith("_import_tmp_"))
            continue;
        const auto source = sourcePath(id);
        items.push_back({id, source, QFileInfo(source).fileName()});
    }
    return items;
}
OperationResult BackupService::addLocal(const QString &source)
{
    if (!QFileInfo::exists(source))
        return OperationResult::fail("添加失败", "源文件或文件夹不存在");
    const auto id = QString::fromUtf8(QUrl::toPercentEncoding(source));
    const auto repo = repoPath(id);
    const auto target = QDir(repo).filePath(QFileInfo(source).fileName());
    ++m_generations[id];
    emit repositoryInvalidated(id);
    if (!QDir().mkpath(repo))
        return OperationResult::fail("添加失败", "无法创建备份仓库");
    if (QFileInfo(source).isFile())
        QFile::remove(target);
    if (!copyPath(source, target))
        return OperationResult::fail("添加失败", "复制源文件失败");
    auto git = runGit(repo, {"init"});
    if (!git.success())
        return OperationResult::fail("初始化失败", git.error);
    runGit(repo, {"config", "user.name", "ZcVersionBox"});
    runGit(repo, {"config", "user.email", "backup@zcversionbox.local"});
    git = runGit(repo, {"add", "."});
    if (!git.success())
        return OperationResult::fail("添加失败", git.error);
    git = runGit(repo, {"commit", "-m", "Initial backup"});
    emit trackedItemsChanged();
    if (!git.success())
        return OperationResult::fail("提交失败", git.error);
    return OperationResult::ok("添加成功", "已将文件添加至版本控制");
}
OperationResult BackupService::backup(const QString &id)
{
    if (!contains(id))
        return missing();
    const auto generation = repositoryGeneration(id);
    const auto source = sourcePath(id);
    if (!QFileInfo::exists(source))
        return OperationResult::fail("自动备份失败", "源文件或文件夹不存在");
    const auto target = QDir(repoPath(id)).filePath(QFileInfo(source).fileName());
    if (!removePath(target) || !copyPath(source, target))
        return OperationResult::fail("自动备份失败", "复制文件失败，请检查权限");
    auto git = runGit(repoPath(id), {"add", "."});
    if (!git.success())
        return OperationResult::fail("自动备份失败", git.error);
    git = runGit(repoPath(id), {"diff", "--cached", "--quiet"});
    if (!git.started || !git.finished)
        return OperationResult::fail("自动备份失败", git.error);
    if (git.exitCode == 0)
        return OperationResult::ok({});
    QString message;
    QSettings settings(m_paths.settingsFile, QSettings::IniFormat);
    if (settings.value("AI/Enabled", settings.value("AI/AutoCommitMessage", false)).toBool())
    {
        auto diff = runGit(repoPath(id), {"diff", "--cached", "--unified=0"});
        if (diff.success() && !diff.output.trimmed().isEmpty())
            message = m_generateMessage(diff.output.trimmed(), m_paths.settingsFile);
    }
    // The synchronous AI helper processes events while waiting. A deleted or rebuilt
    // repository is a different context, even when it uses the same source path.
    if (!contains(id) || generation != repositoryGeneration(id))
        return OperationResult::fail("自动备份已取消", "备份仓库已删除或重建");
    if (message.isEmpty())
        message = "Auto backup - " + QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    git = runGit(repoPath(id), {"commit", "-m", message});
    if (!git.success())
        return OperationResult::fail("自动备份失败", git.error);
    emit repositoryChanged(id);
    return OperationResult::ok({});
}
OperationResult BackupService::statistics(const QString &id, BackupStats &stats) const
{
    stats = {};
    if (!contains(id))
        return missing();
    const QFileInfo source(sourcePath(id));
    stats.sourceState = !source.exists() ? "缺失" : source.isDir() ? "文件夹"
                                                                   : "文件";
    const auto snapshot = pathStats(QDir(repoPath(id)).filePath(source.fileName()));
    stats.fileCount = snapshot.first;
    stats.fileSize = snapshot.second;
    stats.cacheSize = pathStats(repoPath(id) + "/.git/objects").second;
    auto git = runGit(repoPath(id), {"rev-list", "--count", "HEAD"});
    if (git.success())
        stats.versionCount = git.output.trimmed().toInt();
    git = runGit(repoPath(id), {"remote", "get-url", "origin"});
    if (git.success())
        stats.remoteUrl = git.output.trimmed();
    return OperationResult::ok({});
}
OperationResult BackupService::history(const QString &id, QVector<Revision> &revisions) const
{
    revisions.clear();
    if (!contains(id))
        return missing();
    const auto git = runGit(repoPath(id), {"log", "-z", "--format=%h%x00%ct%x00%s"});
    if (!git.success())
        return OperationResult::fail("打开备份失败", git.error);
    // NUL-delimited triples preserve empty subjects and subjects containing
    // spaces. Keep %h and Git's existing ordering; this is presentation data.
    const auto fields = git.output.split(QChar('\0'), Qt::KeepEmptyParts);
    for (qsizetype i = 0; i + 2 < fields.size(); i += 3)
    {
        bool valid = false;
        const auto timestamp = fields[i + 1].toLongLong(&valid);
        if (fields[i].isEmpty() || !valid)
            return OperationResult::fail("打开备份失败", "无法读取提交记录");
        revisions.push_back({fields[i], fields[i + 2], QDateTime::fromSecsSinceEpoch(timestamp, QTimeZone::UTC)});
    }
    return OperationResult::ok({});
}
OperationResult BackupService::diff(const QString &id, const QString &commit, DiffData &data) const
{
    data = {};
    if (!contains(id))
        return missing();
    auto git = runGit(repoPath(id), {"rev-parse", "--verify", commit + "^{commit}"});
    if (!git.success())
        return OperationResult::fail("打开对比失败", git.error);
    data.newCommit = git.output.trimmed();
    git = runGit(repoPath(id), {"rev-parse", data.newCommit + "^"});
    data.oldCommit = git.success() ? git.output.trimmed() : "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
    git = runGit(repoPath(id), {"diff", "--numstat", "-z", data.oldCommit, data.newCommit});
    if (!git.success())
        return OperationResult::fail("打开对比失败", git.error);
    QMap<QString, QString> stats;
    const auto records = git.output.split(QChar('\0'), Qt::KeepEmptyParts);
    for (int i = 0; i < records.size(); ++i)
    {
        const auto firstTab = records[i].indexOf('\t');
        const auto secondTab = records[i].indexOf('\t', firstTab + 1);
        if (firstTab < 0 || secondTab < 0)
            continue;
        const auto added = records[i].left(firstTab);
        const auto removed = records[i].mid(firstTab + 1, secondTab - firstTab - 1);
        auto path = records[i].mid(secondTab + 1);
        // Renames/copies use an empty path followed by old and new NUL-delimited paths.
        if (path.isEmpty() && i + 2 < records.size())
        {
            i += 2;
            path = records[i];
        }
        stats[path] = added == "-" || removed == "-" ? QStringLiteral("二进制") : QString("+%1 / -%2").arg(added, removed);
    }
    git = runGit(repoPath(id), {"diff", "--name-status", "-z", data.oldCommit, data.newCommit});
    if (!git.success())
        return OperationResult::fail("打开对比失败", git.error);
    const auto paths = git.output.split(QChar('\0'), Qt::KeepEmptyParts);
    for (int i = 0; i + 1 < paths.size();)
    {
        const auto status = paths[i++];
        auto path = paths[i++];
        if ((status.startsWith('R') || status.startsWith('C')) && i < paths.size())
            path = paths[i++];
        if (!status.isEmpty() && !path.isEmpty())
            data.files.push_back({status, path, stats.value(path, "-")});
    }
    return OperationResult::ok({});
}
OperationResult BackupService::diffText(const QString &id, const DiffData &data, const QString &file, QString &text) const
{
    if (!contains(id))
        return missing();
    QStringList args{"diff", "--no-color", file.isEmpty() ? "--unified=20" : "--unified=80", data.oldCommit, data.newCommit};
    if (!file.isEmpty())
        args << "--" << file;
    const auto git = runGit(repoPath(id), args);
    text = git.output;
    return git.success() ? OperationResult::ok({}) : OperationResult::fail("加载对比失败", git.error);
}
OperationResult BackupService::preview(const QString &id, const QString &commit)
{
    if (!contains(id))
        return missing();
    auto git = runGit(repoPath(id), {"checkout", "-f", commit});
    if (!git.success())
        return OperationResult::fail("查看失败", git.error);
    const auto target = QDir::tempPath() + "/ZcBox_Preview_" + commit + "_" + id;
    const bool copied = removePath(target) && FileUtils::copyDirectory(repoPath(id), target);
    if (copied)
        FileUtils::setReadOnlyRecursive(target);
    git = runGit(repoPath(id), {"checkout", "-f", "master"});
    if (!copied)
        return OperationResult::fail("查看失败", "复制预览文件失败");
    auto result = OperationResult::ok({});
    result.path = target;
    if (!git.success())
        result.warning = "恢复工作分支失败，请手动执行 git checkout -f master";
    return result;
}
OperationResult BackupService::restore(const QString &id, const QString &commit)
{
    if (!contains(id))
        return missing();
    const auto restored = BackupRestoreHelper::restoreFromGitRevision(repoPath(id), commit, sourcePath(id));
    if (!restored.success)
        return OperationResult::fail("还原失败", restored.errorMessage);
    auto result = OperationResult::ok("还原成功", "已恢复到版本 " + commit);
    result.warning = restored.warningMessage;
    if (!result.warning.isEmpty())
    {
        result.title = "还原完成，但需处理";
        result.duration = 6000;
    }
    emit repositoryChanged(id);
    return result;
}
OperationResult BackupService::editMessage(const QString &id, const QString &commit, const QString &message)
{
    if (!contains(id))
        return missing();
    if (message.isEmpty())
        return OperationResult::fail("错误", "提交说明不能为空");
    auto git = runGit(repoPath(id), {"rev-parse", "HEAD"});
    if (!git.success())
        return OperationResult::fail("保存失败", git.error);
    // Keep the current application's comparison rule. Changing history rewriting is a separate task.
    if (commit != git.output.trimmed())
        return OperationResult::fail("无法编辑", "只能编辑最新的提交说明");
    git = runGit(repoPath(id), {"commit", "--amend", "-m", message});
    if (!git.success())
        return OperationResult::fail("保存失败", git.error);
    emit repositoryChanged(id);
    return OperationResult::ok("已保存", "提交说明已更新");
}
OperationResult BackupService::setRemote(const QString &id, const QString &url)
{
    if (!contains(id))
        return missing();
    if (url.trimmed().isEmpty())
        return OperationResult::fail("云端地址保存失败", "云端地址不能为空");
    const bool exists = runGit(repoPath(id), {"remote", "get-url", "origin"}).success();
    const auto git = runGit(repoPath(id), {"remote", exists ? "set-url" : "add", "origin", url.trimmed()});
    if (!git.success())
        return OperationResult::fail("云端地址保存失败", git.error);
    emit repositoryChanged(id);
    return OperationResult::ok("云端地址已保存", url.trimmed(), 2000);
}
OperationResult BackupService::removeRemote(const QString &id)
{
    if (!contains(id))
        return missing();
    if (runGit(repoPath(id), {"remote", "get-url", "origin"}).success())
    {
        const auto git = runGit(repoPath(id), {"remote", "remove", "origin"});
        if (!git.success())
            return OperationResult::fail("关闭云端同步失败", git.error);
    }
    emit repositoryChanged(id);
    return OperationResult::ok("已关闭云端同步", {}, 2000);
}
OperationResult BackupService::synchronize(const QString &id, bool push)
{
    if (!contains(id))
        return missing();
    const auto git = runGit(repoPath(id), {push ? "push" : "pull", "origin", "master"});
    if (!git.success())
        return OperationResult::fail(push ? "上传失败" : "同步失败", git.error);
    emit repositoryChanged(id);
    return OperationResult::ok(push ? "上传完成" : "已同步", push ? "本地更改已上传到云端" : "已从云端获取最新内容", 2000);
}
OperationResult BackupService::removeBackup(const QString &id)
{
    if (!contains(id))
        return missing();
    ++m_generations[id];
    emit repositoryInvalidated(id);
    if (!QDir(repoPath(id)).removeRecursively())
        return OperationResult::fail("删除备份失败", "删除备份目录失败，请检查权限或文件占用");
    emit trackedItemsChanged();
    return OperationResult::ok("已删除备份", QFileInfo(sourcePath(id)).fileName(), 2000);
}
OperationResult BackupService::rebuild(const QString &id)
{
    if (!contains(id))
        return missing();
    const auto remote = runGit(repoPath(id), {"remote", "get-url", "origin"});
    ++m_generations[id];
    emit repositoryInvalidated(id);
    if (!removePath(repoPath(id) + "/.git"))
        return OperationResult::fail("重建失败", "无法删除旧的 .git 目录");
    auto git = runGit(repoPath(id), {"init"});
    if (!git.success())
        return OperationResult::fail("重建失败", git.error);
    runGit(repoPath(id), {"config", "user.name", "ZcVersionBox"});
    runGit(repoPath(id), {"config", "user.email", "backup@zcversionbox.local"});
    git = runGit(repoPath(id), {"add", "."});
    if (!git.success())
        return OperationResult::fail("重建失败", git.error);
    git = runGit(repoPath(id), {"commit", "-m", "Initial backup"});
    if (!git.success())
        return OperationResult::fail("重建失败", git.error);
    auto result = OperationResult::ok("重建完成", "仓库已重建，历史已清除");
    if (remote.success() && !remote.output.trimmed().isEmpty())
    {
        git = runGit(repoPath(id), {"remote", "add", "origin", remote.output.trimmed()});
        if (!git.success())
            result.warning = "仓库已重建，但云端地址恢复失败，请手动重新配置";
        else if (!runGit(repoPath(id), {"push", "--force", "origin", "master"}).success())
            result.warning = "仓库已重建，但强制推送失败，请检查网络和云端地址";
        else
            result.message += "，云端已同步";
    }
    if (!result.warning.isEmpty())
        result.title = "重建完成（部分）";
    emit repositoryChanged(id);
    return result;
}
OperationResult BackupService::checkRemote(const QString &url) const
{
    if (url.trimmed().isEmpty())
        return OperationResult::fail("检查失败", "请先输入云端仓库地址");
    const auto git = runGit({}, {"ls-remote", "--heads", url.trimmed()});
    return git.success() ? OperationResult::ok("检查成功", "仓库地址可访问", 2000) : OperationResult::fail("检查失败", git.error);
}
PreparedImport BackupService::prepareImport(const QString &url)
{
    if (url.trimmed().isEmpty())
        return {OperationResult::fail("导入失败", "云端仓库地址不能为空")};
    if (!QDir().mkpath(m_paths.backupRoot))
        return {OperationResult::fail("导入失败", "无法创建备份目录")};
    const auto temp = QDir(m_paths.backupRoot).filePath("_import_tmp_" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    m_imports.append(temp);
    const auto git = runGit({}, {"clone", url.trimmed(), temp});
    if (!git.success())
    {
        cancelImport(temp);
        return {OperationResult::fail("导入失败", git.error)};
    }
    const auto entry = importEntry(temp);
    if (!entry.exists())
    {
        cancelImport(temp);
        return {OperationResult::fail("导入失败", "仓库中未找到可导入内容")};
    }
    return {OperationResult::ok({}), temp, entry.fileName(), entry.isDir()};
}
bool BackupService::ownsImport(const QString &path) const { return m_imports.contains(path); }
void BackupService::cancelImport(const QString &temporaryRepo)
{
    if (ownsImport(temporaryRepo))
    {
        QDir(temporaryRepo).removeRecursively();
        m_imports.removeAll(temporaryRepo);
    }
}
OperationResult BackupService::finishImport(const QString &temporaryRepo, const QString &target)
{
    if (!ownsImport(temporaryRepo) || target.isEmpty())
        return OperationResult::fail("导入失败", "导入上下文无效");
    const auto entry = importEntry(temporaryRepo);
    const auto id = QString::fromUtf8(QUrl::toPercentEncoding(target));
    if (contains(id))
    {
        cancelImport(temporaryRepo);
        return OperationResult::fail("导入失败", "该位置已存在追踪记录，请更换位置");
    }
    const auto name = QFileInfo(target).fileName();
    if (name != entry.fileName() && !QDir(temporaryRepo).rename(entry.fileName(), name))
        return OperationResult::fail("导入失败", "重命名导入内容失败");
    if (!QDir().rename(temporaryRepo, repoPath(id)))
        return OperationResult::fail("导入失败", "无法写入备份仓库");
    ++m_generations[id];
    emit repositoryInvalidated(id);
    m_imports.removeAll(temporaryRepo);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !removePath(target) || !copyPath(repoPath(id) + "/" + name, target))
    {
        emit trackedItemsChanged();
        return OperationResult::fail("导入失败", "复制导入内容失败，请检查权限");
    }
    emit trackedItemsChanged();
    return OperationResult::ok("导入成功", "云端备份已加入追踪");
}
