#ifndef SETTINGPAGE_H
#define SETTINGPAGE_H

#include <QWidget>

#include "aiprovider.h"

class ZcStackedWidget;

namespace Ui
{
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
    void on_ToggleSwitch_AutoStart_toggled(bool checked);
    void on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList);
    void on_pushButton_OpenAiPage_clicked();
    void on_lineEdit_AiBaseUrl_editingFinished();
    void on_lineEdit_AiApiKey_editingFinished();
    void on_comboBox_AiModel_currentTextChanged(const QString &text);
    void on_toggleSwitch_AiAutoCommit_toggled(bool checked);
    void on_pushButton_FetchAiModels_clicked();

  private:
    void loadAiSettings();
    void refreshAiModels();
    void applyAiModels(const QList<AiProvider::ModelInfo> &models);

    Ui::SettingPage *ui;
    ZcStackedWidget *m_stackedWidget{nullptr};
    AiProvider *m_aiModelProvider{nullptr};
};

#endif // SETTINGPAGE_H
