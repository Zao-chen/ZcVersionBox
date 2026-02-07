#include "settingpage.h"
#include "ui_settingpage.h"

#include "../Globalconstants.h"

#include <QSettings>
#include <QStandardPaths>
#include <QDir>

SettingPage::SettingPage(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::SettingPage)
{
    ui->setupUi(this);
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
    /*注册表修改*/
    if(checked)
    {
        /*写入右键菜单*/
        QString appPath =
            QCoreApplication::applicationFilePath().replace("/", "\\");
        // 菜单路径（所有文件）
        QString regPath =
            R"(HKEY_CURRENT_USER\Software\Classes\*\shell\ZcVersionOpen)";
        QSettings menu(regPath, QSettings::NativeFormat);
        menu.setValue(".", "添加到VersionBox进行版本控制");
        QSettings command(regPath + R"(\command)", QSettings::NativeFormat);
        command.setValue(".", "\"" + appPath + "\" \"%1\"");
    }
    else
    {
        QSettings settings(
            R"(HKEY_CURRENT_USER\Software\Classes\*\shell)",
            QSettings::NativeFormat);
        settings.remove("ZcVersionOpen");
    }
    /*写入ini进行保存*/
    QSettings ini(Settingpath, QSettings::IniFormat);
    ini.setValue("RightClickMenu", checked);
}

