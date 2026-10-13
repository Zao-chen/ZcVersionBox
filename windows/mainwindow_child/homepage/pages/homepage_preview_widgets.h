#pragma once
#include "utils/source_preview.h"
#include <QPlainTextEdit>
#include <QTextBrowser>

class PreviewTextEdit : public QPlainTextEdit
{
    Q_OBJECT
  public:
    explicit PreviewTextEdit(QWidget *parent = nullptr);
    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
  protected:
    void resizeEvent(QResizeEvent *event) override;
  private:
    QWidget *m_gutter;
    void updateGutter();
};
class PreviewMarkdown : public QTextBrowser
{
    Q_OBJECT
  public:
    explicit PreviewMarkdown(QWidget *parent = nullptr);
    void setContent(const SourceContent &content);
    BackupResult<QString> localTarget(const QUrl &url) const;
  protected:
    QVariant loadResource(int type, const QUrl &name) override;
  private:
    SourceLocation m_location;
};
