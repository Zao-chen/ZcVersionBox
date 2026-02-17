#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <QString>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>

class FileUtils {
public:
    /**
     * @brief 递归复制文件夹
     * @param srcPath 源文件夹路径
     * @param dstPath 目标文件夹路径
     * @return 是否全部复制成功
     */
    static bool copyDirectory(const QString &srcPath, const QString &dstPath);

    /**
     * @brief 确保路径合法化（处理URL编码或非法字符）
     * @param rawPath 原始路径字符串
     * @return 适合作为文件名的字符串
     */
    static QString safeFileName(const QString &rawPath);
    static void setReadOnlyRecursive(const QString &path);
};

#endif // FILEUTILS_H