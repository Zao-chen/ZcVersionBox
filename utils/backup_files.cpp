#include "backup_files.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <filesystem>
#include <functional>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <qt_windows.h>
#endif

namespace
{
OperationResult failed(const QString &what, const QString &path) { return OperationResult::fail("文件操作失败", what + "：" + QDir::toNativeSeparators(path)); }
} // namespace
bool BackupSelectionPolicy::observes(const QString &absolutePath)
{
    const auto path = QDir::fromNativeSeparators(QDir::cleanPath(absolutePath));
    return !path.contains("/.git/") && !path.endsWith("/.git") && !path.contains("/build/") && !path.endsWith("/build");
}
bool BackupSelectionPolicy::copies(const QString &name) { return name.compare(".git", Qt::CaseInsensitive) != 0; }
bool BackupFiles::isLink(const QString &path)
{
    const QFileInfo i(path);
    return i.isSymbolicLink() || i.isJunction();
}
bool BackupFiles::hasLinkedAncestor(const QString &path)
{
    auto current = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    while (!current.isEmpty())
    {
        if (isLink(current))
            return true;
        const auto parent = QFileInfo(current).absolutePath();
        if (parent == current)
            break;
        current = parent;
    }
    return false;
}
bool BackupFiles::isGitMetadataPath(const QString &path)
{
    const auto absolute = QDir::fromNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    for (const auto &part : absolute.split('/'))
        if (!BackupSelectionPolicy::copies(part))
            return true;
    return false;
}
bool BackupFiles::exists(const QString &path) { return QFileInfo::exists(path) || isLink(path); }
BackupResult<QStringList> BackupFiles::children(const QString &path) const
{
    if (hasLinkedAncestor(path))
        return {failed("拒绝遍历链接", path)};
    // QDir::entryInfoList cannot distinguish an empty directory from an I/O
    // failure. Propagate enumeration errors instead of recording false deletions.
    std::error_code error;
#ifdef Q_OS_WIN
    const std::filesystem::path native(path.toStdWString());
#else
    const std::filesystem::path native(QFile::encodeName(path).constData());
#endif
    QStringList result;
    std::filesystem::directory_iterator it(native, error), end;
    while (!error && it != end)
    {
        if (cancellation && cancellation->load())
            return {OperationResult::cancel("操作已取消", path)};
#ifdef Q_OS_WIN
        result.append(QDir::fromNativeSeparators(QString::fromStdWString(it->path().wstring())));
#else
        result.append(QFile::decodeName(it->path().native().c_str()));
#endif
        it.increment(error);
    }
    if (error)
        return {failed("无法完整读取目录（" + QString::fromStdString(error.message()) + "）", path)};
    result.sort();
    return {OperationResult::ok({}), result};
}
bool BackupFiles::overlaps(const QString &first, const QString &second)
{
    const auto normalize = [](const QString &path)
    { const QFileInfo i(path); return QDir::fromNativeSeparators(QDir::cleanPath(i.canonicalFilePath().isEmpty() ? i.absoluteFilePath() : i.canonicalFilePath())); };
    auto a = normalize(first), b = normalize(second);
#ifdef Q_OS_WIN
    a = a.toCaseFolded();
    b = b.toCaseFolded();
#endif
    return a == b || a.startsWith(b.endsWith('/') ? b : b + '/') || b.startsWith(a.endsWith('/') ? a : a + '/');
}
OperationResult BackupFiles::copy(const QString &source, const QString &target) const
{
    if (cancellation && cancellation->load())
        return OperationResult::cancel("操作已取消", source);
    if (hasLinkedAncestor(source) || hasLinkedAncestor(target))
        return failed("不支持复制符号链接或目录联接，未遍历链接目标", source);
    const QFileInfo info(source);
    if (!info.exists() || !info.isReadable())
        return failed("源内容不存在或不可读", source);
    if (info.isDir())
    {
        const auto listed = children(source);
        if (!listed.result.success)
            return listed.result;
        if (!QDir().mkpath(target))
            return failed("无法创建目录", target);
        for (const auto &path : listed.value)
        {
            const QFileInfo entry(path);
            if (!BackupSelectionPolicy::copies(entry.fileName()))
                continue;
            const auto result = copy(entry.filePath(), QDir(target).filePath(entry.fileName()));
            if (!result.success)
                return result;
        }
        return OperationResult::ok({});
    }
    if (!info.isFile())
        return failed("不支持的文件类型", source);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return failed("无法创建父目录", target);
    QFile input(source), output(target);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return failed("无法复制文件", source);
    while (!input.atEnd())
    {
        if (cancellation && cancellation->load())
            return OperationResult::cancel("操作已取消", source);
        const auto bytes = input.read(1024 * 1024);
        if (input.error() != QFile::NoError || output.write(bytes) != bytes.size())
            return failed("读取或写入失败", source);
    }
    if (!output.flush())
        return failed("写入失败", target);
    return output.setPermissions(info.permissions()) ? OperationResult::ok({}) : failed("无法复制文件权限", target);
}
OperationResult BackupFiles::rename(const QString &source, const QString &target) const
{
    if (hasLinkedAncestor(source) || hasLinkedAncestor(target))
        return failed("路径包含链接，已停止替换", source);
    if (exists(target))
        return failed("替换目标已存在", target);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return failed("无法创建父目录", target);
    return QDir().rename(source, target) ? OperationResult::ok({}) : failed("无法移动，请检查文件占用或权限", source);
}
OperationResult BackupFiles::remove(const QString &path) const
{
    if (!exists(path))
        return OperationResult::ok({});
    if (hasLinkedAncestor(path))
        return failed("拒绝递归删除链接", path);
    if (QFileInfo(path).isDir())
    {
        const auto listed = children(path);
        if (!listed.result.success)
            return listed.result;
        for (const auto &entry : listed.value)
        {
            const auto result = remove(entry);
            if (!result.success)
                return result;
        }
        return QDir().rmdir(path) ? OperationResult::ok({}) : failed("无法删除目录", path);
    }
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::WriteOwner);
    return QFile::remove(path) ? OperationResult::ok({}) : failed("无法删除文件", path);
}
bool BackupFiles::openForFingerprint(QFile &file)
{
#ifdef Q_OS_WIN
    // Qt 6's normal QFile reader denies deletion on Windows. A background hash
    // must not prevent an editor's atomic save or a restore's source replacement.
    auto path = QDir::toNativeSeparators(QFileInfo(file.fileName()).absoluteFilePath());
    if (!path.startsWith(QStringLiteral("\\\\?\\")))
        path = path.startsWith(QStringLiteral("\\\\")) ? QStringLiteral("\\\\?\\UNC\\") + path.mid(2) : QStringLiteral("\\\\?\\") + path;
    const auto handle = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor < 0)
    {
        CloseHandle(handle);
        return false;
    }
    if (file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
        return true;
    _close(descriptor);
    return false;
#else
    return file.open(QIODevice::ReadOnly);
#endif
}

OperationResult BackupFiles::fingerprint(const QString &path, bool directory, SourceFingerprint &result, bool observation,
                                         QStringList *visitedPaths) const
{
    result.clear();
    if (visitedPaths)
        visitedPaths->clear();
    const QFileInfo root(path);
    if (hasLinkedAncestor(path) || !root.exists() || root.isDir() != directory || (!directory && !root.isFile()) || !root.isReadable())
        return failed("源不存在、不可读或类型已改变", path);
    std::function<OperationResult(const QString &, const QString &)> scan = [&](const QString &current, const QString &relative)
    {
        if (cancellation && cancellation->load())
            return OperationResult::cancel("操作已取消", current);
        const QFileInfo info(current);
        if (isLink(current))
            return observation ? OperationResult::ok({}) : failed("不支持符号链接或目录联接", current);
        if (!info.exists() || !info.isReadable())
            return failed("无法读取完整源内容", current);
        if (info.isDir())
        {
            if (visitedPaths)
                visitedPaths->append(current);
            const auto listed = children(current);
            if (!listed.result.success)
                return listed.result;
            if (!observation)
                result.insert(relative + '/', "directory");
            for (const auto &path : listed.value)
            {
                const QFileInfo entry(path);
                if (!BackupSelectionPolicy::copies(entry.fileName()) || (observation && !BackupSelectionPolicy::observes(entry.absoluteFilePath())))
                    continue;
                const auto next = scan(entry.filePath(), relative.isEmpty() ? entry.fileName() : relative + '/' + entry.fileName());
                if (!next.success)
                    return next;
            }
        }
        else
        {
            if (!info.isFile())
                return failed("不支持的文件类型", current);
            QFile file(current);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!openForFingerprint(file))
                return failed("文件读取失败", current);
            while (!file.atEnd())
            {
                if (cancellation && cancellation->load())
                    return OperationResult::cancel("操作已取消", current);
                const auto bytes = file.read(1024 * 1024);
                if (file.error() != QFile::NoError)
                    return failed("文件读取失败", current);
                hash.addData(bytes);
            }
            const QFileInfo after(current);
            if (info.size() != after.size() || info.lastModified() != after.lastModified())
                return failed("源内容正在变化，请稍后重试", current);
            auto fingerprint = QString::number(info.size()) + '|' + QString::number(info.lastModified().toMSecsSinceEpoch()) + '|' + QString::fromLatin1(hash.result().toHex());
#ifdef Q_OS_LINUX
            // Git records the owner's executable bit (100644 / 100755).
            // Other permission and ownership metadata are outside Git's model.
            if (info.permission(QFileDevice::ExeOwner) != after.permission(QFileDevice::ExeOwner))
                return failed("源文件执行权限正在变化，请稍后重试", current);
            fingerprint += info.permission(QFileDevice::ExeOwner) ? "|100755" : "|100644";
#endif
            result.insert(relative, fingerprint);
        }
        return OperationResult::ok({});
    };
    return scan(path, directory ? QString() : root.fileName());
}
OperationResult BackupFiles::capture(const QString &source, bool directory, const QString &target, SourceFingerprint &observed) const
{
    SourceFingerprint before, after;
    auto result = fingerprint(source, directory, before, false);
    if (!result.success)
        return result;
    result = fingerprint(source, directory, observed);
    if (!result.success)
        return result;
    result = copy(source, target);
    if (!result.success)
        return result;
    result = fingerprint(source, directory, after, false);
    if (!result.success)
        return result;
    return before == after ? OperationResult::ok({}) : failed("源内容在复制期间发生变化，请重试", source);
}
OperationResult BackupFiles::readOnly(const QString &path) const
{
    if (hasLinkedAncestor(path))
        return failed("拒绝修改链接目标权限", path);
    if (QFileInfo(path).isDir())
    {
        const auto listed = children(path);
        if (!listed.result.success)
            return listed.result;
        for (const auto &entry : listed.value)
        {
            auto r = readOnly(entry);
            if (!r.success)
                return r;
        }
        return OperationResult::ok({});
    }
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther) ? OperationResult::ok({}) : failed("无法设置预览只读权限", path);
}
BackupReplacement::BackupReplacement(std::shared_ptr<BackupFiles> files, QString target, QString stagingParent) : m_files(std::move(files)), m_target(std::move(target))
{
    const auto parent = stagingParent.isEmpty() ? QFileInfo(m_target).absolutePath() : stagingParent;
    if (BackupFiles::hasLinkedAncestor(m_target) || BackupFiles::hasLinkedAncestor(parent))
        return;
    if (!QDir().mkpath(parent))
        return;
    QTemporaryDir temp(QDir(parent).filePath(".zcversionbox-replace-XXXXXX"));
    if (temp.isValid())
    {
        m_root = temp.path();
        temp.setAutoRemove(false);
    }
}
BackupReplacement::~BackupReplacement()
{
    if (!m_preserve && !m_started && !m_root.isEmpty())
        m_files->remove(m_root);
}
BackupResult<bool> BackupReplacement::hasGitMetadata(const QString &path) const
{
    if (BackupFiles::isLink(path) || !QFileInfo(path).isDir())
        return {OperationResult::ok({}), false};
    const auto listed = m_files->children(path);
    if (!listed.result.success)
        return {listed.result};
    for (const auto &entry : listed.value)
    {
        if (!BackupSelectionPolicy::copies(QFileInfo(entry).fileName()))
            return {OperationResult::ok({}), true};
        const auto nested = hasGitMetadata(entry);
        if (!nested.result.success || nested.value)
            return nested;
    }
    return {OperationResult::ok({}), false};
}
OperationResult BackupReplacement::snapshot(const QString &path, SourceFingerprint &contents) const
{
    contents.clear();
    if (!BackupFiles::exists(path))
        return OperationResult::ok({});
    const bool directory = QFileInfo(path).isDir();
    const auto result = m_files->fingerprint(path, directory, contents, false);
    // A same-volume rename changes the root name, not the selected content.
    if (result.success && !directory)
    {
        const auto value = contents.take(QFileInfo(path).fileName());
        contents.insert(QString(), value);
    }
    return result;
}
OperationResult BackupReplacement::matches(const QString &path, const SourceFingerprint &expected) const
{
    SourceFingerprint current;
    const auto scanned = snapshot(path, current);
    if (!scanned.success)
        return scanned;
    return current == expected ? OperationResult::ok({}) : failed("检测到外部修改，已保留副本", m_root);
}
OperationResult BackupReplacement::planMove(const QString &source, const QString &target)
{
    Move move{source, target, {}};
    const auto scanned = snapshot(source, move.contents);
    if (scanned.success)
        m_moves.append(std::move(move));
    return scanned;
}
OperationResult BackupReplacement::plan(const QString &candidate, const QString &target, const QString &relative)
{
    const bool incoming = BackupFiles::exists(candidate), existing = BackupFiles::exists(target);
    if (BackupFiles::isLink(candidate) || BackupFiles::isLink(target))
        return failed("替换路径包含链接", target);
    const auto metadata = existing ? hasGitMetadata(target) : BackupResult<bool>{OperationResult::ok({}), false};
    if (!metadata.result.success)
        return metadata.result;
    if (metadata.value)
    {
        if (incoming && !QFileInfo(candidate).isDir())
            return failed("替换会覆盖源 Git 元数据", target);
        // These directories must remain even when the candidate omits them,
        // because they contain protected source Git metadata.
        m_expected.insert(relative.mid(1) + '/', "directory");
        QMap<QString, QString> names;
        const auto include = [&](const QString &path)
        {
            const auto name = QFileInfo(path).fileName();
            if (!BackupSelectionPolicy::copies(name))
                return;
#ifdef Q_OS_WIN
            names.insert(name.toCaseFolded(), name);
#else
            names.insert(name, name);
#endif
        };
        const auto current = m_files->children(target);
        if (!current.result.success)
            return current.result;
        for (const auto &entry : current.value)
            include(entry);
        if (incoming)
        {
            const auto next = m_files->children(candidate);
            if (!next.result.success)
                return next.result;
            for (const auto &entry : next.value)
                include(entry);
        }
        for (const auto &name : names)
        {
            auto result = plan(QDir(candidate).filePath(name), QDir(target).filePath(name), relative + '/' + name);
            if (!result.success)
                return result;
        }
        return OperationResult::ok({});
    }
    if (existing)
    {
        auto r = planMove(target, m_root + "/old" + relative);
        if (!r.success)
            return r;
    }
    if (incoming)
        return planMove(candidate, target);
    return OperationResult::ok({});
}
OperationResult BackupReplacement::install()
{
    if (!valid() || !BackupFiles::exists(stagingPath()))
        return failed("候选副本不存在", stagingPath());
    auto result = snapshot(m_target, m_before);
    if (!result.success)
        return result;
    SourceFingerprint candidate;
    result = snapshot(stagingPath(), candidate);
    if (!result.success)
        return result;
    m_expected = candidate;
    result = plan(stagingPath(), m_target, {});
    if (!result.success)
        return result;
    result = matches(m_target, m_before);
    if (result.success)
        result = matches(stagingPath(), candidate);
    if (!result.success)
    {
        m_preserve = true;
        return result;
    }
    m_started = true;
    for (const auto &move : m_moves)
    {
        result = matches(move.from, move.contents);
        if (!result.success)
        {
            m_preserve = true;
            return result;
        }
        result = m_files->rename(move.from, move.to);
        if (!result.success)
            return result;
        ++m_applied;
        result = matches(move.to, move.contents);
        if (!result.success)
        {
            m_preserve = true;
            return result;
        }
    }
    result = verifyInstalled();
    if (!result.success)
        m_preserve = true;
    return result;
}
OperationResult BackupReplacement::verifyMoves() const
{
    for (qsizetype i = 0; i < m_applied; ++i)
    {
        const auto result = matches(m_moves[i].to, m_moves[i].contents);
        if (!result.success)
            return result;
    }
    return OperationResult::ok({});
}
OperationResult BackupReplacement::verifyInstalled() const
{
    if (!m_started || m_preserve || m_applied != m_moves.size())
        return failed("无法确认替换后的内容，已保留副本", m_root);
    const auto checked = matches(m_target, m_expected);
    return checked.success ? verifyMoves() : checked;
}
OperationResult BackupReplacement::rollback()
{
    if (m_preserve)
        return failed("检测到外部修改或无法确认内容，未继续回滚；副本保留在", m_root);
    if (m_started)
    {
        const auto checked = verifyMoves();
        if (!checked.success)
        {
            m_preserve = true;
            return checked;
        }
    }
    while (m_applied > 0)
    {
        const auto &move = m_moves[m_applied - 1];
        const auto result = m_files->rename(move.to, move.from);
        if (!result.success)
        {
            m_preserve = true;
            return failed("无法完整回滚，原内容保留在", m_root);
        }
        --m_applied;
    }
    if (m_started)
    {
        const auto restored = matches(m_target, m_before);
        if (!restored.success)
        {
            m_preserve = true;
            return restored;
        }
    }
    m_started = false;
    m_moves.clear();
    return finish();
}
OperationResult BackupReplacement::finish()
{
    m_started = false;
    const auto result = m_files->remove(m_root);
    m_preserve = !result.success;
    if (result.success)
        m_root.clear();
    return result;
}
