#include "settingpage.h"
#include "ui_settingpage.h"

#include "../../../GlobalConstants.h"

#ifdef Q_OS_MACOS
#include "../../../macos/macos_services.h"
#endif

#include "ElaComboBox.h"
#include "ElaMessageBar.h"
#include "ElaToggleSwitch.h"

#include <QDebug>
#include <QSettings>
#include <QSignalBlocker>

#include "zcstackedwidget.h"

namespace
{

QString deriveBaseUrl(const QString &apiUrl)
{
    QString url = apiUrl.trimmed();
    if (url.isEmpty())
        return {};

    if (url.endsWith("/chat/completions"))
    {
        url.chop(QString("/chat/completions").size());
        return url;
    }

    if (url.endsWith("/v1"))
        return url;

    if (url.endsWith("/"))
        url.chop(1);

    return url;
}

} // namespace

SettingPage::SettingPage(QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingPage)
{
    ui->setupUi(this);
    // 一级设置页默认面包屑。
    ui->widget_BreadcrumbBar->setTextPixelSize(25);
    ui->widget_BreadcrumbBar->appendBreadcrumb("设置");

    m_stackedWidget = ui->stackedWidget;
    ui->lineEdit_AiBaseUrl->setClearButtonEnabled(true);
    ui->lineEdit_AiApiKey->setClearButtonEnabled(true);
    ui->comboBox_AiModel->setEditable(true);
    ui->comboBox_AiModel->setInsertPolicy(ElaComboBox::NoInsert);
    if (ui->comboBox_AiModel->lineEdit())
        ui->comboBox_AiModel->lineEdit()->setPlaceholderText("请先获取模型列表");

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

void SettingPage::on_lineEdit_AiBaseUrl_editingFinished()
{
    QSettings ini(Settingpath, QSettings::IniFormat);
    const QString value = ui->lineEdit_AiBaseUrl->text().trimmed();
    ini.setValue("AI/BaseUrl", value);
    qInfo() << "[AI Setting] base URL updated:" << value;
}

void SettingPage::on_lineEdit_AiApiKey_editingFinished()
{
    QSettings ini(Settingpath, QSettings::IniFormat);
    const QString value = ui->lineEdit_AiApiKey->text().trimmed();
    ini.setValue("AI/ApiKey", value);
    qInfo() << "[AI Setting] API key updated, length=" << value.length();
}

void SettingPage::on_comboBox_AiModel_currentTextChanged(const QString &text)
{
    QSettings ini(Settingpath, QSettings::IniFormat);
    const QString value = text.trimmed();
    ini.setValue("AI/Model", value);
    qInfo() << "[AI Setting] model updated:" << value;
}

void SettingPage::on_toggleSwitch_AiAutoCommit_toggled(bool checked)
{
    QSettings ini(Settingpath, QSettings::IniFormat);
    ini.setValue("AI/AutoCommitMessage", checked);
    qInfo() << "[AI Setting] auto AI commit message:" << checked;
}

void SettingPage::on_pushButton_FetchAiModels_clicked()
{
    refreshAiModels();
}

void SettingPage::loadAiSettings()
{
    if (!ui->lineEdit_AiBaseUrl || !ui->lineEdit_AiApiKey || !ui->comboBox_AiModel || !ui->toggleSwitch_AiAutoCommit)
        return;

    QSettings ini(Settingpath, QSettings::IniFormat);
    const QString baseUrl = ini.value("AI/BaseUrl", "https://api.openai.com/v1/chat/completions").toString();
    const QString apiKey = ini.value("AI/ApiKey", QString()).toString();
    const QString model = ini.value("AI/Model", QString()).toString();
    const bool autoCommit = ini.value("AI/AutoCommitMessage", false).toBool();
    qInfo() << "[AI Setting] loaded config, model=" << model << "autoCommit=" << autoCommit;

    {
        QSignalBlocker blocker(ui->lineEdit_AiBaseUrl);
        ui->lineEdit_AiBaseUrl->setText(baseUrl);
    }
    {
        QSignalBlocker blocker(ui->lineEdit_AiApiKey);
        ui->lineEdit_AiApiKey->setText(apiKey);
    }
    {
        QSignalBlocker blocker(ui->comboBox_AiModel);
        ui->comboBox_AiModel->setCurrentText(model);
    }
    {
        QSignalBlocker blocker(ui->toggleSwitch_AiAutoCommit);
        ui->toggleSwitch_AiAutoCommit->setIsToggled(autoCommit);
    }
}

void SettingPage::refreshAiModels()
{
    if (!ui->comboBox_AiModel || !ui->lineEdit_AiBaseUrl || !ui->lineEdit_AiApiKey)
        return;

    const QString baseUrl = ui->lineEdit_AiBaseUrl->text().trimmed();
    const QString apiKey = ui->lineEdit_AiApiKey->text().trimmed();
    if (baseUrl.isEmpty() || apiKey.isEmpty())
    {
        qInfo() << "[AI Setting] skip model refresh: base URL or API key is empty";
        return;
    }

    if (m_aiModelProvider)
    {
        m_aiModelProvider->deleteLater();
        m_aiModelProvider = nullptr;
    }

    m_aiModelProvider = new AiProvider(this);
    m_aiModelProvider->setServiceType(AiProvider::Custom);
    m_aiModelProvider->setApiKey(apiKey);
    m_aiModelProvider->setBaseUrl(deriveBaseUrl(baseUrl));
    qInfo() << "[AI Setting] refreshing models from" << baseUrl;

    connect(m_aiModelProvider, &AiProvider::modelsReceived, this,
            [this](const QList<AiProvider::ModelInfo> &models)
            {
                qInfo() << "[AI Setting] models fetched:" << models.size();
                applyAiModels(models);
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
            });

    m_aiModelProvider->fetchModels();
}

void SettingPage::applyAiModels(const QList<AiProvider::ModelInfo> &models)
{
    if (!ui->comboBox_AiModel)
        return;

    const QString currentModel = ui->comboBox_AiModel->currentText().trimmed();
    QSignalBlocker blocker(ui->comboBox_AiModel);

    ui->comboBox_AiModel->clear();
    for (const auto &model : models)
    {
        if (!model.id.isEmpty())
            ui->comboBox_AiModel->addItem(model.id);
    }

    if (!currentModel.isEmpty())
    {
        int index = ui->comboBox_AiModel->findText(currentModel);
        if (index >= 0)
            ui->comboBox_AiModel->setCurrentIndex(index);
        else
            ui->comboBox_AiModel->setCurrentText(currentModel);
    }
    else if (ui->comboBox_AiModel->count() > 0)
    {
        ui->comboBox_AiModel->setCurrentIndex(0);
        QSettings ini(Settingpath, QSettings::IniFormat);
        ini.setValue("AI/Model", ui->comboBox_AiModel->currentText().trimmed());
    }

    qInfo() << "[AI Setting] model list applied, count=" << ui->comboBox_AiModel->count();
}

void SettingPage::on_pushButton_OpenAiPage_clicked()
{
    if (!m_stackedWidget)
        return;

    qInfo() << "[Setting] navigate to AI subpage";
    m_stackedWidget->setCurrentIndex(1);
    ui->widget_BreadcrumbBar->appendBreadcrumb("AI 配置");
}

void SettingPage::on_widget_BreadcrumbBar_breadcrumbClicked(QString breadcrumb, QStringList lastBreadcrumbList)
{
    Q_UNUSED(breadcrumb)
    Q_UNUSED(lastBreadcrumbList)

    if (!m_stackedWidget)
    {
        return;
    }

    qInfo() << "[Setting] breadcrumb clicked, back to root page";
    m_stackedWidget->setCurrentIndex(0);
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

    QSettings ini(Settingpath, QSettings::IniFormat);
    ini.setValue("AutoStart", checked);
}
