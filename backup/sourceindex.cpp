#include "sourceindex.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <filesystem>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace Backup
{
namespace
{
bool existsStrict(const QString &path)
{
    std::error_code error;
#ifdef Q_OS_WIN
    auto status = std::filesystem::symlink_status(std::filesystem::path(path.toStdWString()), error);
#else
    auto status = std::filesystem::symlink_status(std::filesystem::path(QFile::encodeName(path).constData()), error);
#endif
    if (error && error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory)
        throw Error(ErrorCode::Permission, "无法检查路径，未将其作为删除处理：" + path);
    return std::filesystem::exists(status);
}
} // namespace
QString fileIdentity(const QString &path)
{
#ifdef Q_OS_WIN
    HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (h == INVALID_HANDLE_VALUE)
    {
        const bool denied = GetLastError() == ERROR_ACCESS_DENIED;
        throw Error(denied ? ErrorCode::Permission : ErrorCode::Io, "无法检查文件：" + path, !denied);
    }
    FILE_BASIC_INFO basic{};
    bool ok = GetFileInformationByHandle(h, &info);
    ok = ok && GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof(basic));
    CloseHandle(h);
    if (!ok)
        throw Error(ErrorCode::Io, "无法检查文件身份：" + path, true);
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw Error(ErrorCode::Unsupported, "不支持重解析点或符号链接：" + path);
    return QString("%1:%2:%3:%4:%5:%6:%7:%8").arg(info.dwVolumeSerialNumber).arg(info.nFileIndexHigh).arg(info.nFileIndexLow).arg(info.nFileSizeHigh).arg(info.nFileSizeLow).arg(info.ftLastWriteTime.dwHighDateTime).arg(info.ftLastWriteTime.dwLowDateTime).arg(basic.ChangeTime.QuadPart);
#else
    struct stat s{};
    if (::lstat(QFile::encodeName(path).constData(), &s))
    {
        const bool denied = errno == EACCES || errno == EPERM;
        throw Error(denied ? ErrorCode::Permission : ErrorCode::SourceChanged, "文件已变化或无法访问：" + path, !denied);
    }
    if (S_ISLNK(s.st_mode) || (!S_ISREG(s.st_mode) && !S_ISDIR(s.st_mode)))
        throw Error(ErrorCode::Unsupported, "不支持符号链接或特殊文件：" + path);
#ifdef Q_OS_MACOS
    const auto modified = s.st_mtimespec;
    const auto changed = s.st_ctimespec;
#else
    const auto modified = s.st_mtim;
    const auto changed = s.st_ctim;
#endif
    return QString("%1:%2:%3:%4:%5:%6:%7:%8").arg(s.st_dev).arg(s.st_ino).arg(s.st_size).arg(modified.tv_sec).arg(modified.tv_nsec).arg(changed.tv_sec).arg(changed.tv_nsec).arg(s.st_mode);
#endif
}
FileEntry inspectFile(const QString &path, Cancellation cancel, TaskResult *stats)
{
    const auto identity = fileIdentity(path);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw Error(ErrorCode::Permission, "无法读取文件：" + path);
    FileEntry result;
    result.size = file.size();
#ifndef Q_OS_WIN
    result.executable = QFileInfo(path).permission(QFile::ExeOwner);
#endif
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(QByteArray("blob ") + QByteArray::number(result.size) + '\0');
    qint64 bytes = 0;
    while (bytes < result.size)
    {
        checkCancelled(cancel);
        const auto chunk = file.read(qMin<qint64>(1024 * 1024, result.size - bytes));
        if (chunk.isEmpty())
            throw Error(file.error() == QFile::NoError ? ErrorCode::SourceChanged : ErrorCode::Io, "文件读取中断：" + path, true);
        hash.addData(chunk);
        bytes += chunk.size();
    }
    if (bytes != result.size || fileIdentity(path) != identity)
        throw Error(ErrorCode::SourceChanged, "文件写入尚未完成：" + path, true);
    result.oid = QString::fromLatin1(hash.result().toHex());
    if (stats)
    {
        ++stats->filesRead;
        stats->bytesRead += bytes;
    }
    return result;
}
QString fileIdentityWithin(const QString &source, const QString &path)
{
    if (!containsPath(source, path))
        throw Error(ErrorCode::Unsupported, "文件不在源目录内：" + path);
    QStringList identities{fileIdentity(path)};
    auto parent = path;
    while (!containsPath(parent, source))
    {
        parent = QFileInfo(parent).absolutePath();
        identities.append(fileIdentity(parent));
        if (!QFileInfo(parent).isDir())
            throw Error(ErrorCode::SourceChanged, "父目录已被替换：" + parent, true);
    }
    return identities.join('|');
}
SourceIndex::SourceIndex(QStringList excluded) : m_excluded(std::move(excluded)) {}
bool SourceIndex::excluded(const QString &path) const
{
    for (const auto &part : QDir::fromNativeSeparators(path).split('/'))
        if (part.compare(".git", Qt::CaseInsensitive) == 0)
            return true;
    for (const auto &root : m_excluded)
        if (containsPath(root, path))
            return true;
    return false;
}
void SourceIndex::visit(const QString &source, const QString &relative, Snapshot &s, Cancellation cancel, TaskResult *stats) const
{
    checkCancelled(cancel);
    const auto path = relative.isEmpty() ? source : QDir(source).filePath(relative);
    if (excluded(path))
        return;
    const auto identity = fileIdentity(path);
    const QFileInfo info(path);
    if (info.isFile())
    {
        validateRelative(relative);
        s.files[relative] = inspectFile(path, cancel, stats);
        return;
    }
    if (!info.isDir())
        throw Error(ErrorCode::Unsupported, "不支持的文件类型：" + path);
    if (!info.isReadable())
        throw Error(ErrorCode::Permission, "无法读取目录：" + path);
    if (!relative.isEmpty())
    {
        validateRelative(relative);
        s.directories.append(relative);
    }
    std::error_code error;
#ifdef Q_OS_WIN
    std::filesystem::directory_iterator iterator(std::filesystem::path(path.toStdWString()), error);
#else
    std::filesystem::directory_iterator iterator(std::filesystem::path(QFile::encodeName(path).constData()), error);
#endif
    if (error)
        throw Error(ErrorCode::Permission, "无法枚举目录：" + path);
    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error))
    {
        if (error)
            throw Error(ErrorCode::Io, "目录枚举中断：" + path, true);
#ifdef Q_OS_WIN
        const auto name = QString::fromStdWString(iterator->path().filename().wstring());
#else
        const auto name = QFile::decodeName(iterator->path().filename().native().c_str());
        if (QFile::encodeName(name).toStdString() != iterator->path().filename().native())
            throw Error(ErrorCode::Unsupported, "文件名无法按当前编码表示：" + path);
#endif
        visit(source, relative.isEmpty() ? name : relative + '/' + name, s, cancel, stats);
    }
    if (error || fileIdentity(path) != identity)
        throw Error(ErrorCode::SourceChanged, "目录扫描期间发生变化：" + path, true);
}
Snapshot SourceIndex::scan(const QString &source, const Snapshot *previous, const QStringList &dirty, Cancellation cancel, TaskResult *stats) const
{
    const QFileInfo root(source);
    if (!root.exists() && !root.isSymLink())
        throw Error(ErrorCode::SourceUnavailable, "源路径不可用：" + source);
    fileIdentity(source);
    if (excluded(source))
        throw Error(ErrorCode::Unsupported, "不能备份应用数据或 Git 元数据目录");
    Snapshot result;
    result.name = root.fileName();
    result.kind = root.isDir() ? "directory" : "file";
    if (result.kind == "file")
    {
        result.files["file"] = inspectFile(source, cancel, stats);
        return result;
    }
    if (!previous || previous->kind != result.kind || dirty.isEmpty() || dirty.contains(source))
        visit(source, {}, result, cancel, stats);
    else
    {
        result = *previous;
        QStringList paths;
        for (const auto &path : dirty)
        {
            if (!containsPath(source, path) || excluded(path))
                continue;
            auto relative = QDir(source).relativeFilePath(path);
            if (relative == ".")
                return scan(source, nullptr, {}, cancel, stats);
            validateRelative(relative);
            // A coalesced child event must also reconcile a removed/replaced/new ancestor.
            QString parent;
            const auto parts = relative.split('/');
            for (int i = 0; i + 1 < parts.size(); ++i)
            {
                parent += (parent.isEmpty() ? QString() : "/") + parts[i];
                const auto full = QDir(source).filePath(parent);
                if (!existsStrict(full))
                {
                    relative = parent;
                    break;
                }
                fileIdentity(full); // Reject symbolic links before looking through them.
                if (!QFileInfo(full).isDir() || !previous->directories.contains(parent))
                {
                    relative = parent;
                    break;
                }
            }
            paths.append(relative);
        }
        paths.sort();
        QStringList minimal;
        for (const auto &p : paths)
        {
            bool covered = false;
            for (const auto &ancestor : minimal)
                if (p == ancestor || p.startsWith(ancestor + '/'))
                {
                    covered = true;
                    break;
                }
            if (!covered)
                minimal.append(p);
        }
        for (const auto &p : minimal)
        {
            for (auto it = result.files.begin(); it != result.files.end();)
                if (it.key() == p || it.key().startsWith(p + '/'))
                    it = result.files.erase(it);
                else
                    ++it;
            for (auto it = result.directories.begin(); it != result.directories.end();)
                if (*it == p || it->startsWith(p + '/'))
                    it = result.directories.erase(it);
                else
                    ++it;
            const auto full = QDir(source).filePath(p);
            if (existsStrict(full))
                visit(source, p, result, cancel, stats);
            // A file event can arrive before the corresponding directory event.
            QString parent = p;
            while (parent.contains('/'))
            {
                parent = parent.left(parent.lastIndexOf('/'));
                if (QFileInfo(QDir(source).filePath(parent)).isDir() && !result.directories.contains(parent))
                    result.directories.append(parent);
            }
        }
    }
    result.directories.removeDuplicates();
    result.directories.sort();
    return result;
}
} // namespace Backup
