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

  private:
    Ui::HomePage *ui;
    ElaToggleSwitch *m_ToggleSwitch_Remote{nullptr};
    void LoadBackupFileList();
    void ApplyRemoteUrlFromInput(bool showSuccessMessage);
    QString m_NowFilePathWithCode;
    ElaLineEdit *m_RemoteUrlEdit{nullptr}; //远程仓库地址输入框
    bool m_RemoteUiSyncing{false}; //是否正在进行远程开关，防止反复
};

#endif // HOMEPAGE_H
