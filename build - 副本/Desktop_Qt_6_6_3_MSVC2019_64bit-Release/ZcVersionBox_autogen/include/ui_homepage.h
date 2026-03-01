/********************************************************************************
** Form generated from reading UI file 'homepage.ui'
**
** Created by: Qt User Interface Compiler version 6.6.3
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_HOMEPAGE_H
#define UI_HOMEPAGE_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <elabreadcrumbbar.h>
#include <eladrawerarea.h>
#include <elatableview.h>
#include <elatext.h>
#include <elatoggleswitch.h>
#include <zcstackedwidget.h>

QT_BEGIN_NAMESPACE

class Ui_HomePage
{
public:
    QVBoxLayout *verticalLayout_2;
    ElaBreadcrumbBar *widget_BreadcrumbBar;
    ZcStackedWidget *stackedWidget;
    QWidget *page_0;
    QVBoxLayout *verticalLayout;
    QVBoxLayout *verticalLayout_TrackFiles;
    QWidget *page_1;
    QVBoxLayout *verticalLayout_3;
    QHBoxLayout *horizontalLayout;
    ElaText *label;
    ElaToggleSwitch *ToggleSwitch_Remote;
    QSpacerItem *horizontalSpacer;
    ElaDrawerArea *ElaDrawerArea_Remote;
    ElaTableView *tableView_BackupFiles;

    void setupUi(QWidget *HomePage)
    {
        if (HomePage->objectName().isEmpty())
            HomePage->setObjectName("HomePage");
        HomePage->resize(583, 413);
        verticalLayout_2 = new QVBoxLayout(HomePage);
        verticalLayout_2->setObjectName("verticalLayout_2");
        verticalLayout_2->setContentsMargins(15, 15, 15, 15);
        widget_BreadcrumbBar = new ElaBreadcrumbBar(HomePage);
        widget_BreadcrumbBar->setObjectName("widget_BreadcrumbBar");

        verticalLayout_2->addWidget(widget_BreadcrumbBar);

        stackedWidget = new ZcStackedWidget(HomePage);
        stackedWidget->setObjectName("stackedWidget");
        page_0 = new QWidget();
        page_0->setObjectName("page_0");
        verticalLayout = new QVBoxLayout(page_0);
        verticalLayout->setObjectName("verticalLayout");
        verticalLayout->setContentsMargins(0, 0, 0, 0);
        verticalLayout_TrackFiles = new QVBoxLayout();
        verticalLayout_TrackFiles->setObjectName("verticalLayout_TrackFiles");
        verticalLayout_TrackFiles->setContentsMargins(0, 0, 0, -1);

        verticalLayout->addLayout(verticalLayout_TrackFiles);

        stackedWidget->addWidget(page_0);
        page_1 = new QWidget();
        page_1->setObjectName("page_1");
        verticalLayout_3 = new QVBoxLayout(page_1);
        verticalLayout_3->setSpacing(6);
        verticalLayout_3->setObjectName("verticalLayout_3");
        verticalLayout_3->setContentsMargins(0, 0, 0, 0);
        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setObjectName("horizontalLayout");
        horizontalLayout->setContentsMargins(-1, 0, -1, -1);
        label = new ElaText(page_1);
        label->setObjectName("label");
        QFont font;
        font.setPointSize(12);
        label->setFont(font);

        horizontalLayout->addWidget(label);

        ToggleSwitch_Remote = new ElaToggleSwitch(page_1);
        ToggleSwitch_Remote->setObjectName("ToggleSwitch_Remote");

        horizontalLayout->addWidget(ToggleSwitch_Remote);

        horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Policy::Expanding, QSizePolicy::Policy::Minimum);

        horizontalLayout->addItem(horizontalSpacer);


        verticalLayout_3->addLayout(horizontalLayout);

        ElaDrawerArea_Remote = new ElaDrawerArea(page_1);
        ElaDrawerArea_Remote->setObjectName("ElaDrawerArea_Remote");

        verticalLayout_3->addWidget(ElaDrawerArea_Remote);

        tableView_BackupFiles = new ElaTableView(page_1);
        tableView_BackupFiles->setObjectName("tableView_BackupFiles");
        tableView_BackupFiles->setEditTriggers(QAbstractItemView::NoEditTriggers);
        tableView_BackupFiles->setSelectionMode(QAbstractItemView::NoSelection);
        tableView_BackupFiles->verticalHeader()->setVisible(false);
        tableView_BackupFiles->verticalHeader()->setMinimumSectionSize(50);
        tableView_BackupFiles->verticalHeader()->setDefaultSectionSize(50);

        verticalLayout_3->addWidget(tableView_BackupFiles);

        stackedWidget->addWidget(page_1);

        verticalLayout_2->addWidget(stackedWidget);


        retranslateUi(HomePage);

        stackedWidget->setCurrentIndex(0);


        QMetaObject::connectSlotsByName(HomePage);
    } // setupUi

    void retranslateUi(QWidget *HomePage)
    {
        HomePage->setWindowTitle(QCoreApplication::translate("HomePage", "Form", nullptr));
        label->setText(QCoreApplication::translate("HomePage", "\350\277\234\347\250\213\345\220\214\346\255\245\344\270\216\345\215\217\345\220\214", nullptr));
    } // retranslateUi

};

namespace Ui {
    class HomePage: public Ui_HomePage {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_HOMEPAGE_H
