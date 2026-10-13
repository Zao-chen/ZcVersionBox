#include "homepage_text_edit.h"
#include "windows/mainwindow_presentation.h"
#include <QEvent>
#include <QPainter>
#include <QTextBlock>

namespace
{
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
    : QPlainTextEdit(parent), m_gutter(new TextGutter(this))
{
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFrameShape(QFrame::NoFrame);
    document()->setDocumentMargin(CodeTextStyle::VerticalPadding);
    connect(this, &QPlainTextEdit::blockCountChanged, this, &HomePageTextEdit::updateGutter);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy)
            m_gutter->scroll(0, dy);
        else
            m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
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
    updateGutter();
    viewport()->update();
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
                int(metrics.horizontalAdvance('9') * digits + 2 * CodeTextStyle::HorizontalPadding))
        + CodeTextStyle::BorderWidth;
}

void HomePageTextEdit::updateGutter()
{
    // The document already supplies the vertical padding on all four sides.
    const int inset = CodeTextStyle::HorizontalPadding - CodeTextStyle::VerticalPadding;
    setViewportMargins(gutterWidth() + inset, 0, inset, 0);
    const auto viewportRect = viewport()->geometry();
    m_gutter->setGeometry(contentsRect().left(), viewportRect.top(), gutterWidth(), viewportRect.height());
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
    painter.fillRect(event->rect(), m_colors.gutter);
    painter.fillRect(m_gutter->width() - CodeTextStyle::BorderWidth, event->rect().top(),
                     CodeTextStyle::BorderWidth, event->rect().height(), m_colors.border);
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
            const auto number = QString::number(block.blockNumber() + 1);
            const auto x = m_gutter->width() - CodeTextStyle::BorderWidth
                - CodeTextStyle::HorizontalPadding - numberMetrics.horizontalAdvance(number);
            painter.drawText(x, top + fontMetrics().ascent(), number);
        }
        top += height;
        block = block.next();
    }
}
