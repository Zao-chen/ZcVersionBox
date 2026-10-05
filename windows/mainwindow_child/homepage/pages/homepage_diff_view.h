#pragma once
#include "utils/diff_parser.h"
#include <QWidget>

class QTextBrowser;
class QPlainTextEdit;
class QLabel;
class QStackedWidget;
class QSyntaxHighlighter;

class HomePageDiffView : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePageDiffView(QWidget *parent = nullptr);
    ~HomePageDiffView() override;

    void setDiff(const DiffParser::ParsedDiff &diff, const QString &rawDiff = {},
                 const QString &filePath = {}, const QString &rootPath = {},
                 const QString &oldHeader = QStringLiteral("修改前 (旧版本)"),
                 const QString &newHeader = QStringLiteral("修改后 (当前版本)"));

    void setTexts(const QString &oldText, const QString &newText,
                  const QString &filePath = {}, const QString &rootPath = {},
                  const QString &oldHeader = QStringLiteral("修改前 (旧版本)"),
                  const QString &newHeader = QStringLiteral("修改后 (当前版本)"));

    void setHunk(const QByteArray &beforeBytes, const QByteArray &localBytes,
                 const QByteArray &remoteBytes, const QByteArray &afterBytes,
                 int startLine = 1,
                 const QString &filePath = {}, const QString &rootPath = {},
                 const QString &oldHeader = QStringLiteral("此电脑上的内容 (本地)"),
                 const QString &newHeader = QStringLiteral("云端的内容 (云端)"));

    void showNotice(const QString &title, const QString &desc);
    void clear();
    void refreshTheme();

    QTextBrowser *visualBrowser() const;
    QPlainTextEdit *rawEditor() const;

    QPoint scrollPosition() const;
    void setScrollPosition(const QPoint &pos);

  private:
    QStackedWidget *m_stack{nullptr};
    QWidget *m_visualPage{nullptr};
    QTextBrowser *m_visualBrowser{nullptr};
    QWidget *m_rawPage{nullptr};
    QPlainTextEdit *m_rawEditor{nullptr};
    QSyntaxHighlighter *m_highlighter{nullptr};
    QWidget *m_imagePage{nullptr};
    QLabel *m_imageTitle{nullptr};
    QLabel *m_imageDesc{nullptr};
    QLabel *m_imagePreview{nullptr};

    DiffParser::ParsedDiff m_currentDiff;
    QString m_currentRawDiff;
    QString m_currentFilePath;
    QString m_oldHeader{QStringLiteral("修改前 (旧版本)")};
    QString m_newHeader{QStringLiteral("修改后 (当前版本)")};

    void renderVisualDiff();
};
