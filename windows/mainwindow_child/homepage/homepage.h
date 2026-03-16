#ifndef HOMEPAGE_H
#define HOMEPAGE_H

#include <QWidget>

class ElaLineEdit;
class ElaToggleSwitch;

namespace Ui
{
class HomePage;
}

class HomePage : public QWidget
{
    Q_OBJECT

  public:
    explicit HomePage(QWidget *parent = nullptr);
    void openBackup(QString FilePathWithCode);
    ~HomePage();

  private slots:
    void on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList);
    void on_ToggleSwitch_Remote_toggled(bool checked);

    void on_pushButton_AddFromLoc_clicked();

  private:
    Ui::HomePage *ui;
    ElaToggleSwitch *m_ToggleSwitch_Remote{nullptr};
    void LoadBackupFileList();
    void ApplyRemoteUrlFromInput(bool showSuccessMessage);
    QString m_NowFilePathWithCode;
    ElaLineEdit *m_RemoteUrlEdit{nullptr}; //远程仓库地址输入框
};

#endif // HOMEPAGE_H
