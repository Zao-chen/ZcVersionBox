#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

class FileUtils
{
  public:
    /**
     * @brief 递归复制文件夹
     * @param srcPath 源文件夹路径
     * @param dstPath 目标文件夹路径
     * @return 是否全部复制成功
     */
    static bool copyDirectory(const QString &srcPath, const QString &dstPath);

    static void setReadOnlyRecursive(const QString &path);
};

#endif // FILEUTILS_H