#ifndef HOMEPAGE_H
#define HOMEPAGE_H

#include <QStringList>
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
    void openDiff(QString commitHash);
    ~HomePage();

  private slots:
    void on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList); //面包屑
    void on_ToggleSwitch_Remote_toggled(bool checked);                                                  //打开关闭远程同步
    void on_pushButton_AddFromLoc_clicked();                                                            //添加本地文件
    void on_pushButton_AddFromRemo_clicked();                                                           //从云端导入
    void on_pushButton_DiffAi_clicked();                                                                //AI分析当前对比
    void on_pushButton_DiffBack_clicked();                                                              //返回版本列表

  private:
    Ui::HomePage *ui;
    ElaToggleSwitch *m_ToggleSwitch_Remote{nullptr};
    void SetupTrackFilesPage();
    void SetupBackupPage();
    void SetupDiffPage();
    void LoadBackupFileList();
    void ApplyRemoteUrlFromInput(bool showSuccessMessage);
    void LoadDiffFile(const QString &filePath);
    QString m_NowFilePathWithCode;

    // Diff 页面当前对比上下文，由 openDiff 重置，LoadDiffFile 按需读取。
    QString m_DiffRepoPath;
    QString m_DiffOldCommit;
    QString m_DiffNewCommit;
    QStringList m_DiffFilePaths;
    ElaLineEdit *m_RemoteUrlEdit{nullptr}; //远程仓库地址输入框
};

#endif // HOMEPAGE_H
