/********************************************************************************
** Form generated from reading UI file 'settingpage.ui'
**
** Created by: Qt User Interface Compiler version 6.6.3
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_SETTINGPAGE_H
#define UI_SETTINGPAGE_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <elabreadcrumbbar.h>
#include <elascrollpagearea.h>
#include <elatext.h>
#include <elatoggleswitch.h>

QT_BEGIN_NAMESPACE

class Ui_SettingPage
{
public:
    QVBoxLayout *verticalLayout;
    ElaBreadcrumbBar *widget_BreadcrumbBar;
    ElaScrollPageArea *widget;
    QHBoxLayout *horizontalLayout;
    ElaText *label;
    ElaToggleSwitch *ToggleSwitch_RightClickMenu;
    QSpacerItem *verticalSpacer;

    void setupUi(QWidget *SettingPage)
    {
        if (SettingPage->objectName().isEmpty())
            SettingPage->setObjectName("SettingPage");
        SettingPage->resize(713, 463);
        verticalLayout = new QVBoxLayout(SettingPage);
        verticalLayout->setObjectName("verticalLayout");
        verticalLayout->setContentsMargins(15, 15, 15, 15);
        widget_BreadcrumbBar = new ElaBreadcrumbBar(SettingPage);
        widget_BreadcrumbBar->setObjectName("widget_BreadcrumbBar");

        verticalLayout->addWidget(widget_BreadcrumbBar);

        widget = new ElaScrollPageArea(SettingPage);
        widget->setObjectName("widget");
        horizontalLayout = new QHBoxLayout(widget);
        horizontalLayout->setObjectName("horizontalLayout");
        horizontalLayout->setContentsMargins(12, 12, 12, 12);
        label = new ElaText(widget);
        label->setObjectName("label");
        QFont font;
        font.setPointSize(12);
        label->setFont(font);

        horizontalLayout->addWidget(label);

        ToggleSwitch_RightClickMenu = new ElaToggleSwitch(widget);
        ToggleSwitch_RightClickMenu->setObjectName("ToggleSwitch_RightClickMenu");

        horizontalLayout->addWidget(ToggleSwitch_RightClickMenu);


        verticalLayout->addWidget(widget);

        verticalSpacer = new QSpacerItem(20, 40, QSizePolicy::Policy::Minimum, QSizePolicy::Policy::Expanding);

        verticalLayout->addItem(verticalSpacer);


        retranslateUi(SettingPage);

        QMetaObject::connectSlotsByName(SettingPage);
    } // setupUi

    void retranslateUi(QWidget *SettingPage)
    {
        SettingPage->setWindowTitle(QCoreApplication::translate("SettingPage", "Form", nullptr));
        label->setText(QCoreApplication::translate("SettingPage", "\346\267\273\345\212\240\345\210\260\345\217\263\351\224\256\350\217\234\345\215\225", nullptr));
    } // retranslateUi

};

namespace Ui {
    class SettingPage: public Ui_SettingPage {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_SETTINGPAGE_H
