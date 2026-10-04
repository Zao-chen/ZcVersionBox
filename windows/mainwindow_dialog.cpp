#include "windows/mainwindow_dialog.h"
#include "windows/mainwindow_presentation.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace UiDialog
{
Dialog::Dialog(QWidget *owner, const QString &objectName, const QString &title) : QDialog(owner)
{
    if (!objectName.isEmpty())
        setObjectName(objectName);
    if (!title.isEmpty())
        setWindowTitle(title);
    setWindowModality(owner ? Qt::WindowModal : Qt::ApplicationModal);
    setMinimumWidth(420);
    UiStyle::surface(this, UiStyle::Surface::Popup);

    m_contentLayout = new QVBoxLayout(this);
    m_contentLayout->setContentsMargins(24, 24, 24, 20);
    m_contentLayout->setSpacing(12);
}

QHBoxLayout *Dialog::footerLayout()
{
    auto *footer = new QWidget(this);
    auto *layout = new QHBoxLayout(footer);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    m_contentLayout->addWidget(footer);
    return layout;
}

QDialogButtonBox *Dialog::buttonBox(QDialogButtonBox::StandardButtons buttons)
{
    return new QDialogButtonBox(buttons, this);
}

void Dialog::bindButtonBox(QDialogButtonBox *buttons)
{
    QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void Dialog::prepareAsync()
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowModality(Qt::WindowModal);
}

namespace
{
QLabel *bodyLabel(Dialog &dialog, const QString &text)
{
    auto *label = new QLabel(text, &dialog);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    UiStyle::text(label);
    dialog.contentLayout()->addWidget(label);
    return label;
}

void setAcceptedButton(QDialogButtonBox *buttons, const QString &text)
{
    if (auto *button = buttons->button(QDialogButtonBox::Ok))
    {
        button->setText(text);
        button->setDefault(true);
    }
    else if (auto *button = buttons->button(QDialogButtonBox::Save))
    {
        button->setText(text);
        button->setDefault(true);
    }
}
} // namespace

bool confirm(QWidget *owner, const QString &text, const QString &action)
{
    Dialog dialog(owner, QStringLiteral("confirmDialog"), action);
    dialog.setMinimumWidth(480);
    bodyLabel(dialog, text);

    auto *buttons = dialog.buttonBox(QDialogButtonBox::Cancel);
    auto *confirmButton = buttons->addButton(action, QDialogButtonBox::AcceptRole);
    confirmButton->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setFocus();
    auto *footer = dialog.footerLayout();
    footer->addStretch();
    footer->addWidget(buttons);
    dialog.bindButtonBox(buttons);
    return dialog.exec() == QDialog::Accepted;
}

void showMessage(QWidget *owner, const QString &title, const QString &text, MessageType type)
{
    Dialog dialog(owner, QStringLiteral("messageDialog"), title);
    dialog.setProperty("messageType", static_cast<int>(type));
    bodyLabel(dialog, text);

    auto *buttons = dialog.buttonBox(QDialogButtonBox::Ok);
    setAcceptedButton(buttons, QStringLiteral("确定"));
    auto *footer = dialog.footerLayout();
    footer->addStretch();
    footer->addWidget(buttons);
    dialog.bindButtonBox(buttons);
    dialog.exec();
}

QString getText(QWidget *owner, const QString &title, const QString &label, const QString &defaultText, bool *accepted)
{
    if (accepted)
        *accepted = false;
    Dialog dialog(owner, QStringLiteral("textInputDialog"), title);
    auto *caption = bodyLabel(dialog, label);
    auto *input = new QLineEdit(&dialog);
    input->setText(defaultText);
    input->selectAll();
    input->setAccessibleName(label);
    caption->setBuddy(input);
    dialog.contentLayout()->addWidget(input);

    auto *buttons = dialog.buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    setAcceptedButton(buttons, QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    auto *footer = dialog.footerLayout();
    footer->addStretch();
    footer->addWidget(buttons);
    dialog.bindButtonBox(buttons);
    input->setFocus();
    const auto result = dialog.exec() == QDialog::Accepted;
    if (accepted)
        *accepted = result;
    return input->text();
}
} // namespace UiDialog
