#include "homepage_preview_widgets.h"
#include "windows/mainwindow_presentation.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QTextBlock>

namespace
{
class Gutter : public QWidget
{
  public:
    explicit Gutter(PreviewTextEdit *editor) : QWidget(editor), m_editor(editor) {}
    QSize sizeHint() const override { return {m_editor->gutterWidth(), 0}; }
    void paintEvent(QPaintEvent *event) override { m_editor->paintGutter(event); }
  private:
    PreviewTextEdit *m_editor;
};
}
PreviewTextEdit::PreviewTextEdit(QWidget *parent) : QPlainTextEdit(parent), m_gutter(new Gutter(this))
{
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::NoFrame);
    connect(this, &QPlainTextEdit::blockCountChanged, this, &PreviewTextEdit::updateGutter);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy) m_gutter->scroll(0, dy); else m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
        if (rect.contains(viewport()->rect())) updateGutter();
    });
    updateGutter();
}
int PreviewTextEdit::gutterWidth() const { return 20 + fontMetrics().horizontalAdvance('9') * QString::number(qMax(1, blockCount())).size(); }
void PreviewTextEdit::updateGutter() { setViewportMargins(gutterWidth(), 0, 0, 0); m_gutter->setGeometry(0, 0, gutterWidth(), height()); m_gutter->update(); }
void PreviewTextEdit::resizeEvent(QResizeEvent *event) { QPlainTextEdit::resizeEvent(event); updateGutter(); }
void PreviewTextEdit::paintGutter(QPaintEvent *event)
{
    QPainter painter(m_gutter);
    painter.fillRect(event->rect(), UiStyle::colors().canvas);
    painter.setPen(UiStyle::colors().secondary);
    painter.setFont(font());
    auto block = firstVisibleBlock();
    int number = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= event->rect().bottom())
    {
        const auto height = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && top + height >= event->rect().top())
            painter.drawText(0, top, m_gutter->width() - 8, fontMetrics().height(), Qt::AlignRight, QString::number(number + 1));
        top += height; block = block.next(); ++number;
    }
}
PreviewMarkdown::PreviewMarkdown(QWidget *parent) : QTextBrowser(parent)
{
    setOpenLinks(false); setOpenExternalLinks(false); setFrameShape(QFrame::NoFrame);
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
