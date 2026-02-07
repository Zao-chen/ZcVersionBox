#ifndef SETTINGPAGE_H
#define SETTINGPAGE_H

#include <QWidget>

namespace Ui {
class SettingPage;
}

class SettingPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingPage(QWidget *parent = nullptr);
    ~SettingPage();

private slots:
    void on_ToggleSwitch_RightClickMenu_toggled(bool checked);

private:
    Ui::SettingPage *ui;
};

#endif // SETTINGPAGE_H
