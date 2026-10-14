#pragma once
#include "homepage_text_edit.h"
#include "utils/source_preview.h"
#include <QTextBrowser>

class PreviewTextEdit : public HomePageTextEdit
{
    Q_OBJECT
  public:
    using HomePageTextEdit::HomePageTextEdit;
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
