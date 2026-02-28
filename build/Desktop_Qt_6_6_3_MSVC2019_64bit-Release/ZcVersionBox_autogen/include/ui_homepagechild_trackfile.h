/********************************************************************************
** Form generated from reading UI file 'homepagechild_trackfile.ui'
**
** Created by: Qt User Interface Compiler version 6.6.3
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_HOMEPAGECHILD_TRACKFILE_H
#define UI_HOMEPAGECHILD_TRACKFILE_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QWidget>
#include <elapushbutton.h>
#include <elascrollpagearea.h>
#include <elatext.h>

QT_BEGIN_NAMESPACE

class Ui_HomePageChild_TrackFile
{
public:
    QHBoxLayout *horizontalLayout;
    ElaScrollPageArea *widget;
    QHBoxLayout *horizontalLayout_3;
    ElaText *label;
    ElaPushButton *pushButton_OpenFile;
    ElaPushButton *pushButton_Backup;
    ElaPushButton *pushButton_RemoveTrack;

    void setupUi(QWidget *HomePageChild_TrackFile)
    {
        if (HomePageChild_TrackFile->objectName().isEmpty())
            HomePageChild_TrackFile->setObjectName("HomePageChild_TrackFile");
        HomePageChild_TrackFile->resize(643, 300);
        horizontalLayout = new QHBoxLayout(HomePageChild_TrackFile);
        horizontalLayout->setObjectName("horizontalLayout");
        widget = new ElaScrollPageArea(HomePageChild_TrackFile);
        widget->setObjectName("widget");
        horizontalLayout_3 = new QHBoxLayout(widget);
        horizontalLayout_3->setObjectName("horizontalLayout_3");
        horizontalLayout_3->setContentsMargins(12, 12, 12, 12);
        label = new ElaText(widget);
        label->setObjectName("label");
        QSizePolicy sizePolicy(QSizePolicy::Policy::Preferred, QSizePolicy::Policy::Preferred);
        sizePolicy.setHorizontalStretch(0);
        sizePolicy.setVerticalStretch(0);
        sizePolicy.setHeightForWidth(label->sizePolicy().hasHeightForWidth());
        label->setSizePolicy(sizePolicy);
        QFont font;
        font.setPointSize(12);
        label->setFont(font);

        horizontalLayout_3->addWidget(label);

        pushButton_OpenFile = new ElaPushButton(widget);
        pushButton_OpenFile->setObjectName("pushButton_OpenFile");

        horizontalLayout_3->addWidget(pushButton_OpenFile);

        pushButton_Backup = new ElaPushButton(widget);
        pushButton_Backup->setObjectName("pushButton_Backup");

        horizontalLayout_3->addWidget(pushButton_Backup);

        pushButton_RemoveTrack = new ElaPushButton(widget);
        pushButton_RemoveTrack->setObjectName("pushButton_RemoveTrack");

        horizontalLayout_3->addWidget(pushButton_RemoveTrack);

        horizontalLayout_3->setStretch(0, 2);
        horizontalLayout_3->setStretch(1, 1);
        horizontalLayout_3->setStretch(2, 1);
        horizontalLayout_3->setStretch(3, 1);

        horizontalLayout->addWidget(widget);


        retranslateUi(HomePageChild_TrackFile);

        QMetaObject::connectSlotsByName(HomePageChild_TrackFile);
    } // setupUi

    void retranslateUi(QWidget *HomePageChild_TrackFile)
    {
        HomePageChild_TrackFile->setWindowTitle(QCoreApplication::translate("HomePageChild_TrackFile", "Form", nullptr));
        label->setText(QCoreApplication::translate("HomePageChild_TrackFile", "TextLabel", nullptr));
        pushButton_OpenFile->setText(QCoreApplication::translate("HomePageChild_TrackFile", "\346\211\223\345\274\200\346\226\207\344\273\266", nullptr));
        pushButton_Backup->setText(QCoreApplication::translate("HomePageChild_TrackFile", "\346\237\245\347\234\213\345\244\207\344\273\275", nullptr));
        pushButton_RemoveTrack->setText(QCoreApplication::translate("HomePageChild_TrackFile", "\347\247\273\351\231\244\350\277\275\350\270\252", nullptr));
    } // retranslateUi

};

namespace Ui {
    class HomePageChild_TrackFile: public Ui_HomePageChild_TrackFile {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_HOMEPAGECHILD_TRACKFILE_H
