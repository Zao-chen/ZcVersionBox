#include "homepage_text_edit.h"
#include "windows/mainwindow_presentation.h"
#include <QEvent>
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QTextBlock>
#include <QTextFrame>
#include <QtMath>

// The title band uses the very same HTML as the diff table, including Qt's
// actual font fallback and cell layout instead of approximating them in QLabel.
class HomePageTextHeader : public QWidget
{
  public:
    explicit HomePageTextHeader(QWidget *parent) : QWidget(parent) {}
    QString text;
    void refreshTheme()
    {
        const auto colors = CodeTextStyle::colors(UiStyle::colors().canvas);
        m_document.setDefaultFont(UiStyle::font(UiStyle::FontRole::Body));
        m_document.setDocumentMargin(0);
        m_document.setHtml("<html><head><style>"
            + CodeTextStyle::htmlStyle(colors, UiStyle::font(UiStyle::FontRole::Code).family())
            + CodeTextStyle::headerStyle(colors, UiStyle::colors().secondary)
            + "</style></head><body><table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\">"
              "<tr><th>" + text.toHtmlEscaped() + "</th></tr></table></body></html>");
        update();
    }
    int layoutHeight(int width)
    {
        if (text.isEmpty())
            return 0;
        m_document.setTextWidth(width);
        const auto frames = m_document.rootFrame()->childFrames();
        return frames.isEmpty() ? 0 : qCeil(m_document.documentLayout()->frameBoundingRect(frames.first()).height());
    }

  protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        m_document.drawContents(&painter, event->rect());
    }

  private:
    QTextDocument m_document;
};

namespace
{
class CodeDocumentLayout : public QPlainTextDocumentLayout
{
  public:
    explicit CodeDocumentLayout(QTextDocument *document) : QPlainTextDocumentLayout(document) {}
    QRectF blockBoundingRect(const QTextBlock &block) const override
    {
        auto rect = QPlainTextDocumentLayout::blockBoundingRect(block);
        auto *layout = block.layout();
        qreal top = 0;
        for (int i = 0; i < layout->lineCount(); ++i)
        {
            auto line = layout->lineAt(i);
            line.setPosition({line.x(), top + CodeTextStyle::VerticalPadding});
            top += CodeTextStyle::rowHeight(line.height());
        }
        rect.setTop(0);
        rect.setHeight(top);
        return rect;
    }
};

class TextGutter : public QWidget
{
  public:
    explicit TextGutter(HomePageTextEdit *editor) : QWidget(editor), m_editor(editor) {}
    QSize sizeHint() const override { return {m_editor->gutterWidth(), 0}; }
    void paintEvent(QPaintEvent *event) override { m_editor->paintGutter(event); }

  private:
    HomePageTextEdit *m_editor;
};
}

HomePageTextEdit::HomePageTextEdit(QWidget *parent)
    : QPlainTextEdit(parent), m_gutter(new TextGutter(this)), m_header(new HomePageTextHeader(this))
{
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameStyle(0);
    setFrameShape(QFrame::NoFrame);
    setFrameShadow(QFrame::Plain);
    setLineWidth(0);
    setMidLineWidth(0);
    // Only the layout changes. No block formats are inserted into the document
    // or undo stack, so spacing never becomes an unsaved source modification.
    document()->setDocumentLayout(new CodeDocumentLayout(document()));
    document()->setDocumentMargin(0);
    connect(this, &QPlainTextEdit::blockCountChanged, this, &HomePageTextEdit::updateGutter);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        Q_UNUSED(dy);
        m_gutter->update();
        if (rect.contains(viewport()->rect()))
            updateGutter();
    });
    refreshTheme();
}

void HomePageTextEdit::refreshTheme()
{
    m_colors = CodeTextStyle::colors(UiStyle::colors().canvas);
    setFont(CodeTextStyle::font(UiStyle::font(UiStyle::FontRole::Code)));
    auto codePalette = palette();
    codePalette.setColor(QPalette::Base, m_colors.canvas);
    codePalette.setColor(QPalette::Window, m_colors.canvas);
    codePalette.setColor(QPalette::Text, m_colors.text);
    setPalette(codePalette);
    setAutoFillBackground(true);
    m_header->refreshTheme();
    updateGutter();
    viewport()->update();
}

void HomePageTextEdit::setHeaderText(const QString &text)
{
    m_header->text = text;
    m_header->refreshTheme();
    updateGutter();
}

void HomePageTextEdit::setLineNumbersVisible(bool visible)
{
    m_lineNumbersVisible = visible;
    m_gutter->setVisible(visible);
    updateGutter();
}

int HomePageTextEdit::gutterWidth() const
{
    if (!m_lineNumbersVisible)
        return 0;
    const QFontMetrics metrics(CodeTextStyle::font(font(), true));
    const auto digits = QString::number(qMax(1, blockCount())).size();
    return qMax(CodeTextStyle::MinimumGutterWidth,
                int(metrics.horizontalAdvance('9') * digits + 2 * CodeTextStyle::HorizontalPadding));
}

void HomePageTextEdit::updateGutter()
{
    const int inset = 0;
    const int headerHeight = m_header->layoutHeight(qMax(0, contentsRect().width() - 2 * inset));
    setViewportMargins(inset + gutterWidth() + CodeTextStyle::HorizontalPadding,
                       inset + headerHeight, inset + CodeTextStyle::HorizontalPadding, inset);
    const auto viewportRect = viewport()->geometry();
    m_header->setGeometry(contentsRect().left() + inset, contentsRect().top() + inset,
                          contentsRect().width() - 2 * inset, headerHeight);
    m_header->setVisible(headerHeight > 0);
    m_gutter->setGeometry(contentsRect().left() + inset, viewportRect.top(), gutterWidth(), viewportRect.height());
    m_gutter->update();
}

void HomePageTextEdit::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    updateGutter();
}

void HomePageTextEdit::changeEvent(QEvent *event)
{
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange)
        updateGutter();
}

void HomePageTextEdit::paintEvent(QPaintEvent *event)
{
    // Qlementine leaves text viewports transparent; use the same canvas as diff.
    {
        QPainter painter(viewport());
        painter.fillRect(event->rect(), m_colors.canvas);
    }
    QPlainTextEdit::paintEvent(event);
}

void HomePageTextEdit::paintGutter(QPaintEvent *event)
{
    QPainter painter(m_gutter);
    painter.fillRect(event->rect(), m_colors.canvas);
    painter.setPen(m_colors.lineNumber);
    const auto numberFont = CodeTextStyle::font(font(), true);
    painter.setFont(numberFont);
    const QFontMetrics numberMetrics(numberFont);
    auto block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= event->rect().bottom())
    {
        const auto height = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && top + height >= event->rect().top())
        {
            painter.fillRect(0, top, m_gutter->width(), height, m_colors.gutter);
            painter.fillRect(m_gutter->width() - CodeTextStyle::BorderWidth, top,
                             CodeTextStyle::BorderWidth, height, m_colors.border);
            const auto number = QString::number(block.blockNumber() + 1);
            const auto x = m_gutter->width() - CodeTextStyle::BorderWidth
                - CodeTextStyle::HorizontalPadding - numberMetrics.horizontalAdvance(number);
            const auto line = block.layout()->lineAt(0);
            painter.drawText(x, top + line.y() + numberMetrics.ascent(), number);
        }
        top += height;
        block = block.next();
    }
}
