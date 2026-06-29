#include "settingpage.h"
#include "ui_settingpage.h"

#include "../../../GlobalConstants.h"
#include "../../../utils/aiconfighelper.h"

#ifdef Q_OS_MACOS
#include "../../../macos/macos_services.h"
#endif

#include "ElaComboBox.h"
#include "ElaDrawerArea.h"
#include "ElaMessageBar.h"
#include "ElaPushButton.h"
#include "ElaText.h"
#include "ElaToggleSwitch.h"

#include <QDebug>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QStringListModel>
#include <QVBoxLayout>

#include "zcstackedwidget.h"

namespace AiConfig = AiConfigHelper;

SettingPage::SettingPage(QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingPage)
{
    ui->setupUi(this);
    // 一级设置页默认面包屑。
    ui->widget_BreadcrumbBar->setTextPixelSize(25);
    ui->widget_BreadcrumbBar->appendBreadcrumb("设置");

    m_stackedWidget = ui->stackedWidget;
    setupAiDrawer();
    ui->lineEdit_AiBaseUrl->setClearButtonEnabled(true);
    ui->lineEdit_AiApiKey->setClearButtonEnabled(true);
    m_aiProviderModelListModel = new QStringListModel(this);
    ui->listView_AiModels->setModel(m_aiProviderModelListModel);
    // 模型列表同步到主下拉框。
    connect(ui->listView_AiModels->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current)
            {
                if (m_isLoadingAiSettings || !current.isValid())
                    return;
                const QString model = current.data().toString().trimmed();
                if (!model.isEmpty() && m_aiModelComboBox)
                    m_aiModelComboBox->setCurrentText(model);
            });

    loadAiSettings();
    m_stackedWidget->setCurrentIndex(0);
    qInfo() << "[Setting] initialized with stacked settings pages";

    QSettings ini(Settingpath, QSettings::IniFormat);
    ui->ToggleSwitch_RightClickMenu->setIsToggled(ini.value("RightClickMenu", false).toBool());
    ui->ToggleSwitch_AutoStart->setIsToggled(ini.value("AutoStart", false).toBool());
}

SettingPage::~SettingPage()
{
    delete ui;
}

//加载 AI 设置
void SettingPage::loadAiSettings()
{
    if (!ui->lineEdit_AiBaseUrl || !ui->lineEdit_AiApiKey || !m_aiModelComboBox || !m_aiFeatureSwitch)
        return;

    QSettings ini(Settingpath, QSettings::IniFormat);
    AiConfig::migrateLegacySettings(ini);
    m_currentAiProvider = AiConfig::normalizeProviderName(ini.value("AI/Provider", AiConfig::openAIProviderName()).toString());
    const bool aiEnabled = ini.value("AI/Enabled", ini.value("AI/AutoCommitMessage", false)).toBool();
    qInfo() << "[AI Setting] loaded provider=" << m_currentAiProvider << "aiEnabled=" << aiEnabled;

    loadCurrentProviderSettings();
    updateAiProviderStatuses();
    syncActiveAiConfig();

    {
        QSignalBlocker blocker(m_aiFeatureSwitch);
        m_aiFeatureSwitch->setIsToggled(aiEnabled);
    }
    if (aiEnabled)
        ui->ElaDrawerArea_Ai->expand();
    else
        ui->ElaDrawerArea_Ai->collapse();
}

//设置 AI
void SettingPage::setupAiDrawer()
{
    // 复用云端同步的抽屉样式。
    m_aiFeatureSwitch = new ElaToggleSwitch(this);

    QWidget *drawerHeader = new QWidget(this);
    auto *drawerHeaderLayout = new QHBoxLayout(drawerHeader);
    drawerHeaderLayout->setContentsMargins(8, 0, 8, 0);

    auto *drawerIcon = new ElaText(this);
    drawerIcon->setTextPixelSize(15);
    drawerIcon->setElaIcon(ElaIconType::MessageArrowDown);
    drawerIcon->setFixedSize(25, 25);

    auto *drawerText = new ElaText(QStringLiteral("AI 功能"), this);
    drawerText->setTextPixelSize(15);

    drawerHeaderLayout->addWidget(drawerIcon);
    drawerHeaderLayout->addWidget(drawerText);
    drawerHeaderLayout->addStretch();
    drawerHeaderLayout->addWidget(m_aiFeatureSwitch);
    ui->ElaDrawerArea_Ai->setDrawerHeader(drawerHeader);

    QWidget *drawerContent = new QWidget(this);
    auto *drawerContentLayout = new QVBoxLayout(drawerContent);

    auto *modelRowLayout = new QHBoxLayout();
    auto *modelLabel = new ElaText(QStringLiteral("模型"), drawerContent);
    modelLabel->setTextPixelSize(13);
    m_aiModelComboBox = new ElaComboBox(drawerContent);
    m_aiModelComboBox->setEditable(true);
    m_aiModelComboBox->setInsertPolicy(ElaComboBox::NoInsert);
    if (m_aiModelComboBox->lineEdit())
        m_aiModelComboBox->lineEdit()->setPlaceholderText(QStringLiteral("请先获取模型列表"));

    auto *providerButton = new ElaPushButton(QStringLiteral("服务商设置"), drawerContent);
    providerButton->setMinimumSize(112, 34);

    modelRowLayout->addWidget(modelLabel);
    modelRowLayout->addWidget(m_aiModelComboBox, 1);
    modelRowLayout->addWidget(providerButton);
    drawerContentLayout->addLayout(modelRowLayout);
    ui->ElaDrawerArea_Ai->addDrawer(drawerContent);

    connect(m_aiFeatureSwitch, &ElaToggleSwitch::toggled, this,
            [this](bool checked)
            {
                if (checked)
                    ui->ElaDrawerArea_Ai->expand();
                else
                    ui->ElaDrawerArea_Ai->collapse();
                on_toggleSwitch_AiAutoCommit_toggled(checked);
            });
    connect(ui->ElaDrawerArea_Ai, &ElaDrawerArea::expandStateChanged, this,
            [this](bool isExpand)
            {
                if (m_aiFeatureSwitch->getIsToggled() != isExpand)
                    m_aiFeatureSwitch->setIsToggled(isExpand);
            });
    connect(m_aiModelComboBox, &ElaComboBox::currentTextChanged, this, &SettingPage::on_comboBox_AiModel_currentTextChanged);
    connect(providerButton, &ElaPushButton::clicked, this, &SettingPage::on_pushButton_OpenAiPage_clicked);
}

//选择服务商并进入下一级菜单
void SettingPage::selectAiProvider(const QString &providerName)
{
    if (!m_stackedWidget)
        return;

    m_currentAiProvider = AiConfig::normalizeProviderName(providerName);
    QSettings ini(Settingpath, QSettings::IniFormat);
    ini.setValue("AI/Provider", m_currentAiProvider);

    loadCurrentProviderSettings();
    syncActiveAiConfig();

    m_stackedWidget->setCurrentIndex(2);
    ui->widget_BreadcrumbBar->setBreadcrumbList({QStringLiteral("设置"),
                                                 QStringLiteral("AI 配置"),
                                                 AiConfig::providerDisplayName(m_currentAiProvider)});
    qInfo() << "[AI Setting] select provider:" << m_currentAiProvider;
}

//加载当前服务商设置
void SettingPage::loadCurrentProviderSettings()
{
    if (m_currentAiProvider.isEmpty())
        m_currentAiProvider = AiConfig::openAIProviderName();

    QSettings ini(Settingpath, QSettings::IniFormat);
    const AiConfig::RuntimeConfig config = AiConfig::loadProviderConfig(ini, m_currentAiProvider);

    // 回填 UI 时屏蔽保存信号。
    m_isLoadingAiSettings = true;
    {
        QSignalBlocker blocker(ui->lineEdit_AiApiKey);
        ui->lineEdit_AiApiKey->setText(config.apiKey);
    }
    {
        QSignalBlocker blocker(ui->lineEdit_AiBaseUrl);
        ui->lineEdit_AiBaseUrl->setText(config.baseUrl);
    }
    {
        QSignalBlocker blocker(m_aiModelComboBox);
        m_aiModelComboBox->clear();
        m_aiModelComboBox->addItems(config.modelList);
        m_aiModelComboBox->setCurrentText(config.modelName);
    }
    if (m_aiProviderModelListModel)
    {
        m_aiProviderModelListModel->setStringList(config.modelList);
        const int index = config.modelList.indexOf(config.modelName);
        if (index >= 0)
            ui->listView_AiModels->setCurrentIndex(m_aiProviderModelListModel->index(index));
        else
            ui->listView_AiModels->clearSelection();
    }
    m_isLoadingAiSettings = false;

    ui->widget_AiBaseUrl->setVisible(AiConfig::isCustomProvider(m_currentAiProvider));
    if (m_aiModelComboBox->lineEdit())
        m_aiModelComboBox->lineEdit()->setPlaceholderText(config.modelList.isEmpty() ? QStringLiteral("请先获取模型列表")
                                                                                     : QStringLiteral("请选择模型"));
}

/*更新打勾状态*/
void SettingPage::updateAiProviderStatuses()
{
    QSettings ini(Settingpath, QSettings::IniFormat);
    ui->label_OpenAiProvider_Status->setVisible(AiConfig::isProviderConfigured(ini, AiConfig::openAIProviderName()));
    ui->label_DeepSeekProvider_Status->setVisible(AiConfig::isProviderConfigured(ini, AiConfig::deepSeekProviderName()));
    ui->label_CustomProvider_Status->setVisible(AiConfig::isProviderConfigured(ini, AiConfig::customProviderName()));
}

/*同步到主配置*/
void SettingPage::syncActiveAiConfig()
{
    if (m_currentAiProvider.isEmpty())
        m_currentAiProvider = AiConfig::openAIProviderName();

    QSettings ini(Settingpath, QSettings::IniFormat);
    // 同步给现有 AI 调用入口。
    AiConfig::syncActiveConfig(ini, m_currentAiProvider);
    updateAiProviderStatuses();
}

/*填入url*/
void SettingPage::on_lineEdit_AiBaseUrl_textChanged(const QString &text)
{
    if (m_isLoadingAiSettings || !AiConfig::isCustomProvider(m_currentAiProvider))
        return;

    QSettings ini(Settingpath, QSettings::IniFormat);
    const QString prefix = AiConfig::providerPrefix(m_currentAiProvider);
    ini.setValue(prefix + "BaseUrl", AiConfig::deriveBaseUrl(text));
    AiConfig::clearProviderModels(ini, m_currentAiProvider);
    // 地址变化后清空模型缓存。
    {
        QSignalBlocker blocker(m_aiModelComboBox);
        m_aiModelComboBox->clear();
    }
    if (m_aiProviderModelListModel)
        m_aiProviderModelListModel->setStringList(QStringList());
    syncActiveAiConfig();
    qInfo() << "[AI Setting] custom base URL updated:" << text.trimmed();
}

/*填入 ApiKey*/
void SettingPage::on_lineEdit_AiApiKey_textChanged(const QString &text)
{
    if (m_isLoadingAiSettings || m_currentAiProvider.isEmpty())
        return;

    QSettings ini(Settingpath, QSettings::IniFormat);
    const QString prefix = AiConfig::providerPrefix(m_currentAiProvider);
    ini.setValue(prefix + "ApiKey", text.trimmed());
    AiConfig::clearProviderModels(ini, m_currentAiProvider);
    // Key 变化后清空模型缓存。
    {
        QSignalBlocker blocker(m_aiModelComboBox);
        m_aiModelComboBox->clear();
    }
    if (m_aiProviderModelListModel)
        m_aiProviderModelListModel->setStringList(QStringList());
    syncActiveAiConfig();
    qInfo() << "[AI Setting] API key updated for provider=" << m_currentAiProvider << "length=" << text.trimmed().length();
}

/*切换模型*/
void SettingPage::on_comboBox_AiModel_currentTextChanged(const QString &text)
{
    if (m_isLoadingAiSettings || m_currentAiProvider.isEmpty())
        return;

    QSettings ini(Settingpath, QSettings::IniFormat);
    const QString value = text.trimmed();
    ini.setValue(AiConfig::providerPrefix(m_currentAiProvider) + "Model", value);
    if (m_aiProviderModelListModel)
    {
        const QStringList modelList = m_aiProviderModelListModel->stringList();
        const int index = modelList.indexOf(value);
        if (index >= 0)
            ui->listView_AiModels->setCurrentIndex(m_aiProviderModelListModel->index(index));
    }
    syncActiveAiConfig();
    qInfo() << "[AI Setting] model updated for provider=" << m_currentAiProvider << "model=" << value;
}

/*自动ai生成提交开关*/
void SettingPage::on_toggleSwitch_AiAutoCommit_toggled(bool checked)
{
    QSettings ini(Settingpath, QSettings::IniFormat);
    ini.setValue("AI/Enabled", checked);
    ini.setValue("AI/AutoCommitMessage", checked);
    qInfo() << "[AI Setting] AI feature enabled:" << checked;
}

/*刷新服务商模型列表*/
void SettingPage::on_pushButton_FetchAiModels_clicked()
{
    if (!m_aiModelComboBox || !ui->lineEdit_AiBaseUrl || !ui->lineEdit_AiApiKey)
        return;

    if (m_currentAiProvider.isEmpty())
        m_currentAiProvider = AiConfig::openAIProviderName();

    const QString baseUrl = AiConfig::isCustomProvider(m_currentAiProvider)
                                ? ui->lineEdit_AiBaseUrl->text().trimmed()
                                : QString();
    const QString apiKey = ui->lineEdit_AiApiKey->text().trimmed();
    if ((AiConfig::isCustomProvider(m_currentAiProvider) && baseUrl.isEmpty()) || apiKey.isEmpty())
    {
        qInfo() << "[AI Setting] skip model refresh: base URL or API key is empty";
        ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                               "模型获取失败",
                               "请先填写当前服务商的 API Key 和接口地址",
                               3000,
                               this);
        return;
    }

    if (m_aiModelProvider)
    {
        m_aiModelProvider->deleteLater();
        m_aiModelProvider = nullptr;
    }

    m_aiModelProvider = new AiProvider(this);
    m_aiModelProvider->setServiceType(AiConfig::serviceTypeForProvider(m_currentAiProvider));
    m_aiModelProvider->setApiKey(apiKey);
    // Custom 才需要自定义地址。
    if (AiConfig::isCustomProvider(m_currentAiProvider))
        m_aiModelProvider->setBaseUrl(AiConfig::deriveBaseUrl(baseUrl));
    m_fetchingAiProvider = m_currentAiProvider;
    qInfo() << "[AI Setting] refreshing models from provider=" << m_fetchingAiProvider << "baseUrl=" << baseUrl;

    connect(m_aiModelProvider, &AiProvider::modelsReceived, this,
            [this](const QList<AiProvider::ModelInfo> &models)
            {
                qInfo() << "[AI Setting] models fetched:" << models.size();

                /*添加模型列表*/

                if (!m_aiModelComboBox)
                    return;

                const QString providerName = m_fetchingAiProvider.isEmpty() ? m_currentAiProvider : m_fetchingAiProvider;
                const QString currentModel = m_aiModelComboBox->currentText().trimmed();
                QStringList modelIds;
                QSignalBlocker blocker(m_aiModelComboBox);

                // 更新列表和主下拉框。
                m_aiModelComboBox->clear();
                for (const auto &model : models)
                {
                    if (!model.id.isEmpty())
                    {
                        m_aiModelComboBox->addItem(model.id);
                        modelIds.append(model.id);
                    }
                }
                if (m_aiProviderModelListModel)
                    m_aiProviderModelListModel->setStringList(modelIds);

                if (!currentModel.isEmpty())
                {
                    int index = m_aiModelComboBox->findText(currentModel);
                    if (index >= 0)
                        m_aiModelComboBox->setCurrentIndex(index);
                    else
                        m_aiModelComboBox->setCurrentText(currentModel);
                }
                else if (m_aiModelComboBox->count() > 0)
                {
                    m_aiModelComboBox->setCurrentIndex(0);
                }

                QSettings ini(Settingpath, QSettings::IniFormat);
                const QString prefix = AiConfig::providerPrefix(providerName);
                ini.setValue(prefix + "ModelList", modelIds);
                ini.setValue(prefix + "Model", m_aiModelComboBox->currentText().trimmed());
                const int selectedIndex = modelIds.indexOf(m_aiModelComboBox->currentText().trimmed());
                if (m_aiProviderModelListModel && selectedIndex >= 0)
                    ui->listView_AiModels->setCurrentIndex(m_aiProviderModelListModel->index(selectedIndex));
                if (providerName == m_currentAiProvider)
                    syncActiveAiConfig();

                qInfo() << "[AI Setting] model list applied, count=" << m_aiModelComboBox->count();

                m_fetchingAiProvider.clear();
                m_aiModelProvider->deleteLater();
                m_aiModelProvider = nullptr;
            });
    connect(m_aiModelProvider, &AiProvider::errorOccurred, this,
            [this](const QString &error)
            {
                qInfo() << "[AI Setting] model refresh failed:" << error;
                ElaMessageBar::warning(ElaMessageBarType::BottomRight,
                                       "模型获取失败",
                                       error,
                                       3500,
                                       this);
                if (m_aiModelProvider)
                {
                    m_aiModelProvider->deleteLater();
                    m_aiModelProvider = nullptr;
                }
                m_fetchingAiProvider.clear();
            });

    m_aiModelProvider->fetchModels();
}

void SettingPage::on_pushButton_OpenAiPage_clicked()
{
    if (!m_stackedWidget)
        return;

    qInfo() << "[Setting] navigate to AI subpage";
    m_stackedWidget->setCurrentIndex(1);
    ui->widget_BreadcrumbBar->setBreadcrumbList({QStringLiteral("设置"),
                                                 QStringLiteral("AI 配置")});
    updateAiProviderStatuses();
}

/*不同模型提供商进入下级*/
void SettingPage::on_pushButton_OpenAiProvider_Set_clicked()
{
    selectAiProvider(AiConfig::openAIProviderName());
}

void SettingPage::on_pushButton_DeepSeekProvider_Set_clicked()
{
    selectAiProvider(AiConfig::deepSeekProviderName());
}

void SettingPage::on_pushButton_CustomProvider_Set_clicked()
{
    selectAiProvider(AiConfig::customProviderName());
}

/*返回上级*/
void SettingPage::on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList)
{
    Q_UNUSED(lastBreadcrumbList)

    if (!m_stackedWidget)
    {
        return;
    }

    if (breadcrumb == QStringLiteral("AI 配置"))
    {
        qInfo() << "[Setting] breadcrumb clicked, back to AI provider page";
        m_stackedWidget->setCurrentIndex(1);
        ui->widget_BreadcrumbBar->setBreadcrumbList({QStringLiteral("设置"),
                                                     QStringLiteral("AI 配置")});
        updateAiProviderStatuses();
        return;
    }

    qInfo() << "[Setting] breadcrumb clicked, back to root page";
    m_stackedWidget->setCurrentIndex(0);
    ui->widget_BreadcrumbBar->setBreadcrumbList({QStringLiteral("设置")});
}

/*右键菜单*/
void SettingPage::on_ToggleSwitch_RightClickMenu_toggled(bool checked)
{
#ifdef Q_OS_WIN
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
#elif defined(Q_OS_MACOS)
    setMacServicesProviderEnabled(checked);
    ElaMessageBar::information(ElaMessageBarType::BottomRight,
                               "右键菜单快捷入口",
                               checked ? "已启用 Finder 服务：添加到 ZcVersionBox"
                                       : "已停用 Finder 服务",
                               3000,
                               this);
#endif
    QSettings ini(Settingpath, QSettings::IniFormat);
    ini.setValue("RightClickMenu", checked);
}

/*开机自启*/
void SettingPage::on_ToggleSwitch_AutoStart_toggled(bool checked)
{
#ifdef Q_OS_WIN
    const QString runRegPath = R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)";
    const QString runName = QStringLiteral("ZcVersionBox");
    QString appPath = QCoreApplication::applicationFilePath().replace("/", "\\");

    QSettings runSetting(runRegPath, QSettings::NativeFormat);
    if (checked)
    {
        runSetting.setValue(runName, "\"" + appPath + "\"");
    }
    else
    {
        runSetting.remove(runName);
    }
#elif defined(Q_OS_MACOS)
    const bool success = setMacAutoStartEnabled(checked);
    if (!success)
    {
        QSignalBlocker blocker(ui->ToggleSwitch_AutoStart);
        ui->ToggleSwitch_AutoStart->setIsToggled(!checked);
        ElaMessageBar::error(ElaMessageBarType::BottomRight,
                             "开机自启设置失败",
                             "无法写入用户 LaunchAgent",
                             3000,
                             this);
        return;
    }
    ElaMessageBar::information(ElaMessageBarType::BottomRight,
                               "开机自启",
                               checked ? "已启用 macOS 登录时自动启动"
                                       : "已停用 macOS 登录时自动启动",
                               3000,
                               this);
#endif

    QSettings ini(Settingpath, QSettings::IniFormat);
    ini.setValue("AutoStart", checked);
}
