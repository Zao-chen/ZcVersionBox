#pragma once
#include "utils/code_text_style.h"
#include <QPlainTextEdit>

class HomePageTextEdit : public QPlainTextEdit
{
    Q_OBJECT
  public:
    explicit HomePageTextEdit(QWidget *parent = nullptr);
    void refreshTheme();
    void setLineNumbersVisible(bool visible);
    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

  private:
    QWidget *m_gutter;
    CodeTextStyle::Colors m_colors;
    bool m_lineNumbersVisible{true};
    void updateGutter();
};
