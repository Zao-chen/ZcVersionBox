#include "fileutils.h"
#include <QUrl>

bool FileUtils::copyDirectory(const QString &srcPath, const QString &dstPath)
{
    QDir srcDir(srcPath);
    if (!srcDir.exists())
    {
        qDebug() << "Source directory does not exist:" << srcPath;
        return false;
    }

    QDir dstDir(dstPath);
    if (!dstDir.exists())
    {
        // mkpath 会递归创建所有不存在的父级目录
        if (!dstDir.mkpath("."))
            return false;
    }

    // 1. 复制所有文件
    QStringList files = srcDir.entryList(QDir::Files | QDir::Hidden);
    for (const QString &file : files)
    {
        QString srcFile = srcPath + "/" + file;
        QString dstFile = dstPath + "/" + file;

        if (QFile::exists(dstFile))
        {
            QFile::remove(dstFile);
        }
        if (!QFile::copy(srcFile, dstFile))
            return false;
    }

    // 2. 递归复制子文件夹
    QStringList dirs = srcDir.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &dir : dirs)
    {
        // Git 元数据不是用户快照内容，显式排除，避免预览仓库时复制整个对象库。
        if (dir == QStringLiteral(".git"))
            continue;

        QString srcSub = srcPath + "/" + dir;
        QString dstSub = dstPath + "/" + dir;
        if (!copyDirectory(srcSub, dstSub))
            return false;
    }

    return true;
}

QString FileUtils::safeFileName(const QString &rawPath)
{
    // 如果 rawPath 包含 %3A 等编码，先解码
    // 然后将原本可能引起歧义的字符替换掉，或者只取最后一段
    QString decoded = QUrl::fromPercentEncoding(rawPath.toUtf8());

    // 替换掉 Windows 不允许的文件名字符
    QString safe = decoded;
    safe.replace(":", "_").replace("\\", "_").replace("/", "_");
    return safe;
}

// FileUtils.cpp 增加设置只读的逻辑
void FileUtils::setReadOnlyRecursive(const QString &path)
{
    QDir dir(path);
    // 处理文件
    QStringList files = dir.entryList(QDir::Files | QDir::Hidden);
    for (const QString &file : files)
    {
        QString filePath = path + "/" + file;
        // 设置为只读权限 (0x444)
        QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
    // 递归处理子文件夹
    QStringList dirs = dir.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &subDir : dirs)
    {
        setReadOnlyRecursive(path + "/" + subDir);
    }
}
