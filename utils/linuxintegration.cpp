#include "linuxintegration.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <utility>

namespace
{
OperationResult manageFile(const QString &path, const QByteArray &prefix, const QByteArray &content, bool enabled, bool executable)
{
    const QFileInfo existing(path);
    if (existing.isSymLink() || (existing.exists() && !existing.isFile()))
        return OperationResult::fail("设置失败", "入口路径不是普通文件：" + path);
    if (existing.exists())
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || !file.readAll().startsWith(prefix))
            return OperationResult::fail("设置失败", "已有文件不属于 ZcVersionBox，已保留：" + path);
    }
    if (!enabled)
        return !existing.exists() || QFile::remove(path) ? OperationResult::ok("设置已更新", "已停用")
                                                       : OperationResult::fail("设置失败", "无法删除入口：" + path);
    if (!QDir().mkpath(existing.absolutePath()))
        return OperationResult::fail("设置失败", "无法创建入口目录：" + existing.absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size())
        return OperationResult::fail("设置失败", "无法写入入口：" + path);
    auto permissions = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
    if (executable)
        permissions |= QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther;
    if (!file.setPermissions(permissions) || !file.commit())
        return OperationResult::fail("设置失败", "无法保存入口或执行权限：" + path);
    return OperationResult::ok("设置已更新", "已启用");
}
QString desktopArgument(const QString &value)
{
    QString result = "\"";
    for (const auto ch : value)
    {
        if (ch == '\\')
            result += QStringLiteral("\\\\\\\\");
        else if (ch == '"' || ch == '$' || ch == '`')
            result += QStringLiteral("\\\\") + ch;
        else if (ch == '%')
            result += "%%";
        else
            result += ch;
    }
    return result + '"';
}
bool validExecutable(const QString &value)
{
    return QDir::isAbsolutePath(value) && !value.contains('\n') && !value.contains('\r') && !value.contains(QChar(0));
}
} // namespace

LinuxIntegrationPaths LinuxIntegrationPaths::defaults()
{
    return {QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation),
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation),
            QCoreApplication::applicationFilePath()};
}
LinuxIntegration::LinuxIntegration(LinuxIntegrationPaths paths) : m_paths(std::move(paths)) {}
OperationResult LinuxIntegration::setAutoStart(bool enabled) const
{
    if (!QDir::isAbsolutePath(m_paths.configHome) || (enabled && !validExecutable(m_paths.executable)))
        return OperationResult::fail("设置失败", "自启动目录或应用路径无效");
    const QByteArray prefix = "[Desktop Entry]\nX-ZcVersionBox-Managed=1\n";
    const auto content = prefix + "Type=Application\nName=ZcVersionBox\nExec=" + desktopArgument(m_paths.executable).toUtf8() +
                         "\nIcon=zcversionbox\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
    return manageFile(QDir(m_paths.configHome).filePath("autostart/com.zc.versionbox.desktop"), prefix, content, enabled, false);
}
OperationResult LinuxIntegration::setNautilusScript(bool enabled) const
{
    if (!QDir::isAbsolutePath(m_paths.dataHome) || (enabled && !validExecutable(m_paths.executable)))
        return OperationResult::fail("设置失败", "脚本目录或应用路径无效");
    const QByteArray prefix = "#!/bin/sh\n# ZcVersionBox managed integration v1\n";
    auto executable = m_paths.executable;
    executable.replace(QChar(0x27), QStringLiteral("'\\''"));
    const auto content = prefix + "exec '" + executable.toUtf8() + "' --nautilus-add\n";
    return manageFile(QDir(m_paths.dataHome).filePath(QStringLiteral("nautilus/scripts/添加到 ZcVersionBox")),
                      prefix, content, enabled, true);
}
OperationResult LinuxIntegration::nautilusSelection(const QByteArray &uris, QStringList &paths)
{
    paths.clear();
    QStringList selected;
    for (const auto &entry : uris.split('\n'))
    {
        const auto encoded = entry.trimmed();
        if (encoded.isEmpty())
            continue;
        const auto url = QUrl::fromEncoded(encoded, QUrl::StrictMode);
        const auto path = url.toLocalFile();
        if (!url.isValid() || !url.isLocalFile() || (!url.host().isEmpty() && url.host() != "localhost") ||
            url.hasQuery() || url.hasFragment() || !QDir::isAbsolutePath(path) || path.contains(QChar(0)))
            return OperationResult::fail("添加失败", "请选择本地文件或文件夹，远程文件位置暂不支持");
        if (!selected.contains(path))
            selected.append(path);
    }
    if (selected.isEmpty())
        return OperationResult::fail("添加失败", "请先在文件管理器中选择文件或文件夹");
    paths = selected;
    return OperationResult::ok({});
}
