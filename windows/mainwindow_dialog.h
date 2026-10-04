#pragma once

#include <QDialog>
#include <QDialogButtonBox>

class QHBoxLayout;
class QLineEdit;
class QVBoxLayout;

namespace UiDialog
{
class Dialog final : public QDialog
{
  public:
    explicit Dialog(QWidget *owner = nullptr, const QString &objectName = {}, const QString &title = {});

    QVBoxLayout *contentLayout() const { return m_contentLayout; }
    QHBoxLayout *footerLayout();
    QDialogButtonBox *buttonBox(QDialogButtonBox::StandardButtons buttons);
    void bindButtonBox(QDialogButtonBox *buttons);
    void prepareAsync();

  private:
    QVBoxLayout *m_contentLayout;
};

enum class MessageType
{
    Information,
    Warning,
    Error
};

bool confirm(QWidget *owner, const QString &text, const QString &action);
void showMessage(QWidget *owner, const QString &title, const QString &text, MessageType type = MessageType::Information);
QString getText(QWidget *owner, const QString &title, const QString &label, const QString &defaultText, bool *accepted);
} // namespace UiDialog
