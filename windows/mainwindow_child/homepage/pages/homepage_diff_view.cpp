#include "homepage_diff_view.h"
#include "windows/mainwindow_presentation.h"
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSpacerItem>
#include <QStackedWidget>
#include <QSyntaxHighlighter>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QVBoxLayout>

namespace
{
class DiffHighlighter : public QSyntaxHighlighter
{
  public:
    using QSyntaxHighlighter::QSyntaxHighlighter;

  protected:
    void highlightBlock(const QString &line) override
    {
        const auto colors = UiStyle::colors();
        QTextCharFormat format;
        if (line.startsWith("diff --git") || line.startsWith("index ") || line.startsWith("@@") ||
            line.startsWith("--- ") || line.startsWith("+++ ") || line.startsWith("\\ ") || line.startsWith("Binary files "))
            format.setForeground(colors.secondary);
        else if (line.startsWith('+'))
            format.setForeground(colors.added);
        else if (line.startsWith('-'))
            format.setForeground(colors.removed);
        else
            return;
        setFormat(0, line.size(), format);
    }
};
} // namespace

HomePageDiffView::HomePageDiffView(QWidget *parent)
    : QWidget(parent)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName("diffStack");
    mainLayout->addWidget(m_stack);

    // 1. Visual Page
    m_visualPage = new QWidget(m_stack);
    auto *visualLayout = new QVBoxLayout(m_visualPage);
    visualLayout->setContentsMargins(0, 0, 0, 0);
    visualLayout->setSpacing(0);

    m_visualBrowser = new QTextBrowser(m_visualPage);
    m_visualBrowser->setObjectName("visualBrowser");
    m_visualBrowser->setFrameShape(QFrame::NoFrame);
    m_visualBrowser->setOpenExternalLinks(false);
    visualLayout->addWidget(m_visualBrowser);
    m_stack->addWidget(m_visualPage);

    // 2. Raw Page
    m_rawPage = new QWidget(m_stack);
    auto *rawLayout = new QVBoxLayout(m_rawPage);
    rawLayout->setContentsMargins(0, 0, 0, 0);
    rawLayout->setSpacing(0);

    m_rawEditor = new QPlainTextEdit(m_rawPage);
    m_rawEditor->setObjectName("content");
    m_rawEditor->setReadOnly(true);
    m_rawEditor->setFrameShape(QFrame::NoFrame);
    m_rawEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
    UiStyle::text(m_rawEditor, UiStyle::FontRole::Code);
    m_highlighter = new DiffHighlighter(m_rawEditor->document());
    rawLayout->addWidget(m_rawEditor);
    m_stack->addWidget(m_rawPage);

    // 3. Image / Notice Page
    m_imagePage = new QWidget(m_stack);
    auto *imageLayout = new QVBoxLayout(m_imagePage);
    imageLayout->setContentsMargins(16, 24, 16, 24);
    imageLayout->setSpacing(12);

    m_imageTitle = new QLabel(m_imagePage);
    m_imageTitle->setObjectName("imageNoticeTitle");
    m_imageTitle->setAlignment(Qt::AlignCenter);
    UiStyle::text(m_imageTitle, UiStyle::FontRole::Section, false);
    imageLayout->addWidget(m_imageTitle);

    m_imageDesc = new QLabel(m_imagePage);
    m_imageDesc->setObjectName("imageNoticeDesc");
    m_imageDesc->setAlignment(Qt::AlignCenter);
    UiStyle::text(m_imageDesc, UiStyle::FontRole::Caption, true);
    imageLayout->addWidget(m_imageDesc);

    m_imagePreview = new QLabel(m_imagePage);
    m_imagePreview->setObjectName("imagePreview");
    m_imagePreview->setAlignment(Qt::AlignCenter);
    imageLayout->addWidget(m_imagePreview);

    imageLayout->addSpacerItem(new QSpacerItem(0, 0, QSizePolicy::Minimum, QSizePolicy::Expanding));
    m_stack->addWidget(m_imagePage);

    m_stack->setCurrentWidget(m_visualPage);
}

HomePageDiffView::~HomePageDiffView() = default;

void HomePageDiffView::setDiff(const DiffParser::ParsedDiff &diff, const QString &rawDiff,
                               const QString &filePath, const QString &rootPath,
                               const QString &oldHeader, const QString &newHeader)
{
    m_currentDiff = diff;
    m_currentRawDiff = rawDiff;
    m_currentFilePath = filePath;
    m_oldHeader = oldHeader.isEmpty() ? QStringLiteral("修改前 (旧版本)") : oldHeader;
    m_newHeader = newHeader.isEmpty() ? QStringLiteral("修改后 (当前版本)") : newHeader;

    const QString lowerPath = filePath.toLower();
    const bool isImage = lowerPath.endsWith(".png") || lowerPath.endsWith(".jpg") ||
                         lowerPath.endsWith(".jpeg") || lowerPath.endsWith(".webp") ||
                         lowerPath.endsWith(".svg") || lowerPath.endsWith(".ico") ||
                         lowerPath.endsWith(".bmp");

    if (m_currentDiff.isBinary)
    {
        m_stack->setCurrentWidget(m_imagePage);
        if (isImage)
        {
            m_imageTitle->setText(QStringLiteral("图片文件变更 · %1").arg(QFileInfo(filePath).fileName()));
            m_imageDesc->setText(QStringLiteral("此文件为图像格式，已记录当前版本变更。"));

            QString fullPath = filePath;
            if (!rootPath.isEmpty() && !fullPath.startsWith('/') && !fullPath.contains(':'))
                fullPath = rootPath + "/" + filePath;

            QPixmap pixmap(fullPath);
            if (!pixmap.isNull())
            {
                if (pixmap.width() > 400 || pixmap.height() > 300)
                    pixmap = pixmap.scaled(400, 300, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                m_imagePreview->setPixmap(pixmap);
            }
            else
            {
                m_imagePreview->setText(QStringLiteral("(无法直接加载本地图像预览)"));
            }
        }
        else
        {
            m_imageTitle->setText(QStringLiteral("二进制文件 · %1").arg(QFileInfo(filePath).fileName()));
            m_imageDesc->setText(m_currentDiff.binaryNotice.isEmpty() ? QStringLiteral("二进制文件无法直接显示文本对比。") : m_currentDiff.binaryNotice);
            m_imagePreview->clear();
        }
        return;
    }

    if (m_rawEditor->toPlainText() != m_currentRawDiff)
        m_rawEditor->setPlainText(m_currentRawDiff);

    m_stack->setCurrentWidget(m_visualPage);
    renderVisualDiff();
}

void HomePageDiffView::setTexts(const QString &oldText, const QString &newText,
                                const QString &filePath, const QString &rootPath,
                                const QString &oldHeader, const QString &newHeader)
{
    const auto diff = DiffParser::diffTexts(oldText, newText);
    setDiff(diff, QString(), filePath, rootPath, oldHeader, newHeader);
}

void HomePageDiffView::setHunk(const QByteArray &beforeBytes, const QByteArray &localBytes,
                               const QByteArray &remoteBytes, const QByteArray &afterBytes,
                               int startLine,
                               const QString &filePath, const QString &rootPath,
                               const QString &oldHeader, const QString &newHeader)
{
    const auto diff = DiffParser::diffHunk(beforeBytes, localBytes, remoteBytes, afterBytes, startLine);
    setDiff(diff, QString(), filePath, rootPath, oldHeader, newHeader);
}

void HomePageDiffView::showNotice(const QString &title, const QString &desc)
{
    m_stack->setCurrentWidget(m_imagePage);
    m_imageTitle->setText(title);
    m_imageDesc->setText(desc);
    m_imagePreview->clear();
}

void HomePageDiffView::clear()
{
    m_visualBrowser->clear();
    m_rawEditor->clear();
    m_imageTitle->clear();
    m_imageDesc->clear();
    m_imagePreview->clear();
    m_currentDiff = {};
    m_currentRawDiff.clear();
    m_currentFilePath.clear();
}

void HomePageDiffView::refreshTheme()
{
    if (m_highlighter)
        m_highlighter->rehighlight();
    renderVisualDiff();
}

QTextBrowser *HomePageDiffView::visualBrowser() const
{
    return m_visualBrowser;
}

QPlainTextEdit *HomePageDiffView::rawEditor() const
{
    return m_rawEditor;
}

QPoint HomePageDiffView::scrollPosition() const
{
    if (!m_visualBrowser)
        return {};
    return {m_visualBrowser->horizontalScrollBar()->value(),
            m_visualBrowser->verticalScrollBar()->value()};
}

void HomePageDiffView::setScrollPosition(const QPoint &pos)
{
    if (!m_visualBrowser)
        return;
    m_visualBrowser->horizontalScrollBar()->setValue(pos.x());
    m_visualBrowser->verticalScrollBar()->setValue(pos.y());
}

void HomePageDiffView::renderVisualDiff()
{
    if (!m_visualBrowser)
        return;

    const auto colors = UiStyle::colors();
    const QString codeFontFamily = UiStyle::font(UiStyle::FontRole::Code).family();

    DiffParser::RenderColors renderColors;
    renderColors.canvas = colors.canvas;
    renderColors.surface = colors.sidebar;
    renderColors.text = colors.text;
    renderColors.secondaryText = colors.secondary;
    renderColors.border = colors.separator;

    renderColors.addedBg = colors.added;
    renderColors.addedText = colors.added;
    renderColors.addedWordBg = colors.added;

    renderColors.removedBg = colors.removed;
    renderColors.removedText = colors.removed;
    renderColors.removedWordBg = colors.removed;

    renderColors.headerBg = colors.sidebar;
    renderColors.emptyBg = colors.sidebar;

    const QString html = DiffParser::renderHtml(m_currentDiff, DiffParser::ViewMode::SideBySide, renderColors, codeFontFamily, m_oldHeader, m_newHeader);
    m_visualBrowser->setHtml(html);
}
