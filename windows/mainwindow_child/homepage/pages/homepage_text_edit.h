#pragma once
#include "utils/code_text_style.h"
#include <QPlainTextEdit>

class HomePageTextHeader;

class HomePageTextEdit : public QPlainTextEdit
{
    Q_OBJECT
  public:
    explicit HomePageTextEdit(QWidget *parent = nullptr);
    void refreshTheme();
    void setLineNumbersVisible(bool visible);
    void setHeaderText(const QString &text);
    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    QWidget *m_gutter;
    HomePageTextHeader *m_header;
    CodeTextStyle::Colors m_colors;
    bool m_lineNumbersVisible{true};
    void updateGutter();
};
