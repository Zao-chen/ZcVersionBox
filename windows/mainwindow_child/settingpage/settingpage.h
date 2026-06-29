#ifndef SETTINGPAGE_H
#define SETTINGPAGE_H

#include <QString>
#include <QWidget>

#include "AiProvider.h"

class ElaComboBox;
class ElaToggleSwitch;
class QStringListModel;
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
    void on_pushButton_OpenAiProvider_Set_clicked();
    void on_pushButton_DeepSeekProvider_Set_clicked();
    void on_pushButton_CustomProvider_Set_clicked();
    void on_lineEdit_AiBaseUrl_textChanged(const QString &text);
    void on_lineEdit_AiApiKey_textChanged(const QString &text);
    void on_comboBox_AiModel_currentTextChanged(const QString &text);
    void on_toggleSwitch_AiAutoCommit_toggled(bool checked);
    void on_pushButton_FetchAiModels_clicked();

  private:
    // AI 设置流程：
    // loadAiSettings -> loadCurrentProviderSettings -> syncActiveAiConfig
    // selectAiProvider -> loadCurrentProviderSettings -> syncActiveAiConfig
    // refreshAiModels -> applyAiModels -> syncActiveAiConfig

    // 创建主设置页里的 AI 抽屉。
    void setupAiDrawer();

    // 初始化 AI 配置和抽屉开关状态。
    void loadAiSettings();

    // 进入某个服务商的配置页。
    void selectAiProvider(const QString &providerName);

    // 把当前服务商配置回填到 UI。
    void loadCurrentProviderSettings();

    // 刷新服务商列表里的完成状态标记。
    void updateAiProviderStatuses();

    // 把当前服务商配置同步到 AI/* 运行时入口。
    void syncActiveAiConfig();

    // 调用 ZcAILib 获取当前服务商模型列表。
    void refreshAiModels();

    // 保存模型列表，并同步列表和主下拉框。
    void applyAiModels(const QList<AiProvider::ModelInfo> &models);

    Ui::SettingPage *ui;
    ZcStackedWidget *m_stackedWidget{nullptr};
    AiProvider *m_aiModelProvider{nullptr};
    ElaToggleSwitch *m_aiFeatureSwitch{nullptr};
    ElaComboBox *m_aiModelComboBox{nullptr};
    QStringListModel *m_aiProviderModelListModel{nullptr};
    QString m_currentAiProvider;
    QString m_fetchingAiProvider;
    bool m_isLoadingAiSettings{false};
};

#endif // SETTINGPAGE_H
