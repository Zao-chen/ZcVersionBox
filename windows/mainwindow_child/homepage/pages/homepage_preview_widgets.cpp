#include "homepage_preview_widgets.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>

PreviewMarkdown::PreviewMarkdown(QWidget *parent) : QTextBrowser(parent)
{
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setFrameStyle(0);
    setFrameShape(QFrame::NoFrame);
    setFrameShadow(QFrame::Plain);
    setLineWidth(0);
    setMidLineWidth(0);
}
void PreviewMarkdown::setContent(const SourceContent &content)
{
    clear();
    m_location = content.location;
    const auto path = m_location.path.isEmpty() ? m_location.root : QDir(m_location.root).filePath(m_location.path);
    document()->setBaseUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath() + '/'));
    document()->setMarkdown(content.text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
}
BackupResult<QString> PreviewMarkdown::localTarget(const QUrl &url) const
{
    const auto resolved = document()->baseUrl().resolved(url);
    if (!resolved.isLocalFile()) return {OperationResult::fail("不支持此链接", {})};
    auto target = m_location;
    target.path = target.rootDirectory ? QDir(target.root).relativeFilePath(resolved.toLocalFile()) : QString();
    if (!target.rootDirectory && QFileInfo(resolved.toLocalFile()) != QFileInfo(target.root)) return {OperationResult::fail("链接超出追踪范围", {})};
    const auto checked = sourcePreviewPath(target);
    return {checked.result, target.path};
}
QVariant PreviewMarkdown::loadResource(int type, const QUrl &url)
{
    if (type != QTextDocument::ImageResource) return {};
    const auto target = localTarget(url);
    if (!target.result.success) return {};
    const auto path = target.value.isEmpty() ? m_location.root : QDir(m_location.root).filePath(target.value);
    if (QFileInfo(path).size() > SourcePreview::ImageLimit) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(SourcePreview::ImageLimit + 1);
    if (bytes.size() > SourcePreview::ImageLimit) return {};
    return decodeSourceContent(bytes).image;
}
