#include "source_preview.h"
#include "backup_files.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QStringConverter>

BackupResult<QString> sourcePreviewPath(const SourceLocation &location, bool mustExist)
{
    const auto fail = [] { return BackupResult<QString>{OperationResult::fail("路径不可用", "路径必须位于追踪范围内，且不能包含 .git、链接或目录跳转。")}; };
    const auto relative = location.path;
    if (location.root.isEmpty() || (!location.rootDirectory && !relative.isEmpty()) ||
        QDir::isAbsolutePath(relative) || relative.contains('\\') || relative.contains(QChar(0))) return fail();
    if (!relative.isEmpty())
        for (const auto &part : relative.split('/'))
            if (part.isEmpty() || part == "." || part == ".." || part.compare(".git", Qt::CaseInsensitive) == 0) return fail();
    const auto path = relative.isEmpty() ? location.root : QDir(location.root).filePath(relative);
    if (BackupFiles::hasLinkedAncestor(path) || BackupFiles::isGitMetadataPath(path)) return fail();
    const QFileInfo root(location.root), info(path);
    if (!root.exists() || root.isDir() != location.rootDirectory || (!root.isDir() && !root.isFile())) return fail();
    if (mustExist && (!info.exists() || (!info.isDir() && !info.isFile())))
        return {OperationResult::fail("源内容不可用", "文件或文件夹已不存在，或不是普通文件。")};
    return {OperationResult::ok({}), path};
}
BackupResult<QByteArray> sourcePreviewStamp(const QString &path)
{
    QFileInfo before(path);
    if (!before.isFile() || BackupFiles::hasLinkedAncestor(path))
        return {OperationResult::fail("文件不可用", path)};
    QFile file(path);
    if (!BackupFiles::openForFingerprint(file)) return {OperationResult::fail("无法读取文件", file.errorString())};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {OperationResult::fail("无法读取完整文件", path)};
    QFileInfo after(path);
    if (before.size() != after.size() || before.lastModified() != after.lastModified() || before.birthTime() != after.birthTime() ||
        before.permissions() != after.permissions() || BackupFiles::hasLinkedAncestor(path))
        return {OperationResult::fail("文件正在变化", "请稍后重新读取。")};
    return {OperationResult::ok({}), hash.result().toHex() + ':' + QByteArray::number(after.size()) + ':' +
        QByteArray::number(after.lastModified().toMSecsSinceEpoch()) + ':' + QByteArray::number(after.birthTime().toMSecsSinceEpoch()) + ':' + QByteArray::number(after.permissions())};
}
SourceContent decodeSourceContent(const QByteArray &bytes)
{
    SourceContent content;
    content.bytes = bytes;
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto size = reader.size();
    if (size.isValid())
    {
        if (bytes.size() > SourcePreview::ImageLimit || qint64(size.width()) * size.height() > SourcePreview::PixelLimit)
            content.notice = "图片超出预览上限，请用系统应用打开。";
        else
        {
            content.image = reader.read();
            if (content.image.isNull()) content.notice = "无法解码图片，请用系统应用打开。";
        }
        return content;
    }
    if (bytes.size() > SourcePreview::TextLimit)
    {
        content.notice = "文件超出文本预览上限（2 MiB），请用系统应用打开。";
        return content;
    }
    auto encoding = QStringConverter::Utf8;
    QByteArray input = bytes;
    if (input.startsWith(QByteArray::fromHex("efbbbf"))) { content.bom = true; input.remove(0, 3); }
    else if (input.startsWith(QByteArray::fromHex("fffe"))) { content.bom = true; encoding = QStringConverter::Utf16LE; content.encoding = "UTF-16LE"; input.remove(0, 2); }
    else if (input.startsWith(QByteArray::fromHex("feff"))) { content.bom = true; encoding = QStringConverter::Utf16BE; content.encoding = "UTF-16BE"; input.remove(0, 2); }
    QStringDecoder decoder(encoding, QStringConverter::Flag::Stateless);
    content.text = decoder(input);
    bool controls = false;
    for (const auto c : content.text)
        if (c.unicode() < 32 && c != '\n' && c != '\r' && c != '\t') { controls = true; break; }
    if (decoder.hasError() || controls)
    {
        content.text.clear(); content.notice = "此文件不是受支持的文本编码，请用系统应用打开。"; return content;
    }
    if (content.text.count('\n') + 1 > SourcePreview::LineLimit)
    {
        content.text.clear(); content.notice = "文件超过 20,000 行，请用系统应用打开。"; return content;
    }
    content.textual = true;
    const int crlf = content.text.count("\r\n"), lf = content.text.count('\n'), cr = content.text.count('\r');
    content.editable = cr == crlf && (crlf == 0 || crlf == lf);
    content.newline = crlf ? "\r\n" : "\n";
    if (!content.editable) content.notice = "文件含混合换行，仅供查看；请用系统应用编辑。";
    content.text.replace("\r\n", "\n");
    return content;
}
