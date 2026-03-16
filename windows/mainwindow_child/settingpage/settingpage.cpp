#include "settingpage.h"
#include "ui_settingpage.h"

#include "../../../Globalconstants.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>


SettingPage::SettingPage(QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingPage)
{
    ui->setupUi(this);
    //创建面包屑
    QStringList breadcrumbBarList;
    ui->widget_BreadcrumbBar->setTextPixelSize(25);
    ui->widget_BreadcrumbBar->appendBreadcrumb("设置");
    //读取ini并初始化配置项
    QSettings ini(Settingpath, QSettings::IniFormat);
    ui->ToggleSwitch_RightClickMenu->setIsToggled(ini.value("RightClickMenu", false).toBool());
}

SettingPage::~SettingPage()
{
    delete ui;
}

void SettingPage::on_ToggleSwitch_RightClickMenu_toggled(bool checked)
{
    QString appPath = QCoreApplication::applicationFilePath().replace("/", "\\");
    QString menuText = QStringLiteral("使用ZcVersionBox自动备份");

    if (checked)
    {
        //所有文件
        {
            QString regPath = R"(HKEY_CURRENT_USER\Software\Classes\*\shell\ZcVersionOpen)";
            QSettings menu(regPath, QSettings::NativeFormat);
            menu.setValue(".", menuText);

            QSettings command(regPath + R"(\command)", QSettings::NativeFormat);
            command.setValue(".", "\"" + appPath + "\" \"%1\"");
        }
        //文件夹（右键点文件夹对象）
        {
            QString regPath = R"(HKEY_CURRENT_USER\Software\Classes\Directory\shell\ZcVersionOpen)";
            QSettings menu(regPath, QSettings::NativeFormat);
            menu.setValue(".", menuText);

            QSettings command(regPath + R"(\command)", QSettings::NativeFormat);
            command.setValue(".", "\"" + appPath + "\" \"%1\"");
        }
        //文件夹背景（进入文件夹后空白处右键）
        {
            QString regPath = R"(HKEY_CURRENT_USER\Software\Classes\Directory\Background\shell\ZcVersionOpen)";
            QSettings menu(regPath, QSettings::NativeFormat);
            menu.setValue(".", menuText);

            QSettings command(regPath + R"(\command)", QSettings::NativeFormat);
            command.setValue(".", "\"" + appPath + "\" \"%V\"");
        }
    }
    else
    {
        //移除：所有文件
        {
            QSettings settings(R"(HKEY_CURRENT_USER\Software\Classes\*\shell)", QSettings::NativeFormat);
            settings.remove("ZcVersionOpen");
        }
        //移除：文件夹对象
        {
            QSettings settings(R"(HKEY_CURRENT_USER\Software\Classes\Directory\shell)", QSettings::NativeFormat);
            settings.remove("ZcVersionOpen");
        }
        //移除：文件夹背景
        {
            QSettings settings(R"(HKEY_CURRENT_USER\Software\Classes\Directory\Background\shell)", QSettings::NativeFormat);
            settings.remove("ZcVersionOpen");
        }
    }
    QSettings ini(Settingpath, QSettings::IniFormat);
    ini.setValue("RightClickMenu", checked);
}
