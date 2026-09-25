#include "settingsPage.h"
#include "ui_aiSettingsPage.h"
#include "ui_generalSettingsPage.h"
#include <QSignalBlocker>
#include <oclero/qlementine/widgets/Switch.hpp>

GeneralSettingsPage::GeneralSettingsPage(SettingsService *settings, QWidget *parent) : QWidget(parent), ui(new Ui::GeneralSettingsPage)
{
    ui->setupUi(this);
    auto *rightClick = new oclero::qlementine::Switch(this);
    rightClick->setObjectName("rightClickSwitch");
    rightClick->setAccessibleName("右键菜单快捷入口");
    auto *autoStart = new oclero::qlementine::Switch(this);
    autoStart->setObjectName("autoStartSwitch");
    autoStart->setAccessibleName("开机自动启动");
    rightClick->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    autoStart->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    rightClick->setFocusPolicy(Qt::StrongFocus);
    autoStart->setFocusPolicy(Qt::StrongFocus);
    ui->rightClickLayout->addWidget(rightClick);
    ui->autoStartLayout->addWidget(autoStart);
    const auto reload = [=]
    {
        QSignalBlocker b1(rightClick), b2(autoStart);
        rightClick->setChecked(settings->value("RightClickMenu", false).toBool());
        autoStart->setChecked(settings->value("AutoStart", false).toBool());
    };
    reload();
    connect(settings, &SettingsService::changed, this, reload);
    connect(rightClick, &QAbstractButton::toggled, this, [=](bool checked)
            {
                emit notification(settings->setSystemOption("RightClickMenu", checked));
                reload();
            });
    connect(autoStart, &QAbstractButton::toggled, this, [=](bool checked)
            {
                emit notification(settings->setSystemOption("AutoStart", checked));
                reload();
            });
    connect(ui->aiButton, &QPushButton::clicked, this, [this]
            {
                emit navigate({PageId::AiSettings});
            });
}
GeneralSettingsPage::~GeneralSettingsPage() = default;
AiSettingsPage::AiSettingsPage(SettingsService *settings, QWidget *parent) : QWidget(parent), ui(new Ui::AiSettingsPage), m_settings(settings)
{
    ui->setupUi(this);
    ui->provider->addItems({"OpenAI", "DeepSeek", "Custom"});
    ui->model->setEditable(true);
    ui->model->setInsertPolicy(QComboBox::NoInsert);
    ui->apiKey->setEchoMode(QLineEdit::Password);
    m_enabled = new oclero::qlementine::Switch(this);
    m_enabled->setObjectName("aiEnabledSwitch");
    m_enabled->setAccessibleName("AI 自动提交说明");
    m_enabled->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_enabled->setFocusPolicy(Qt::StrongFocus);
    ui->enabledLayout->addWidget(m_enabled);
    ui->enabledLayout->setStretch(0, 1);
    connect(m_enabled, &QAbstractButton::toggled, this, [this](bool enabled)
            {
                if (!m_loading)
                    m_settings->setAiEnabled(enabled);
            });
    connect(ui->provider, &QComboBox::currentTextChanged, this, [this](const QString &name)
            {
                if (!m_loading)
                {
                    m_settings->selectProvider(name);
                    emit navigate({PageId::AiSettings, {}, {}, name});
                }
            });
    connect(ui->apiKey, &QLineEdit::textChanged, this, [this](const QString &text)
            {
                if (!m_loading)
                    m_settings->saveField("ApiKey", text);
            });
    connect(ui->baseUrl, &QLineEdit::textChanged, this, [this](const QString &text)
            {
                if (!m_loading)
                    m_settings->saveField("BaseUrl", text);
            });
    connect(ui->model, &QComboBox::currentTextChanged, this, [this](const QString &text)
            {
                if (!m_loading)
                    m_settings->saveField("Model", text);
            });
    connect(ui->models, &QListWidget::currentTextChanged, this, [this](const QString &text)
            {
                if (!m_loading && !text.isEmpty())
                    m_settings->saveField("Model", text);
            });
    connect(ui->fetchButton, &QPushButton::clicked, settings, &SettingsService::fetchModels);
    connect(settings, &SettingsService::changed, this, &AiSettingsPage::refresh);
    connect(settings, &SettingsService::fetchingChanged, this, [this](bool fetching)
            {
                ui->fetchButton->setEnabled(!fetching);
                ui->fetchButton->setText(fetching ? "正在获取…" : "获取模型列表");
            });
    connect(ui->generalButton, &QPushButton::clicked, this, [this]
            {
                emit navigate({PageId::GeneralSettings});
            });
    refresh();
}
AiSettingsPage::~AiSettingsPage() = default;
void AiSettingsPage::refresh()
{
    m_loading = true;
    const auto config = m_settings->config(m_settings->provider());
    const bool providerChanged = config.providerName != m_displayedProvider;
    m_displayedProvider = config.providerName;
    m_enabled->setChecked(m_settings->value("AI/Enabled", m_settings->value("AI/AutoCommitMessage", false)).toBool());
    ui->provider->setCurrentText(config.providerName);
    if (providerChanged || !ui->apiKey->hasFocus())
        ui->apiKey->setText(config.apiKey);
    if (providerChanged || !ui->baseUrl->hasFocus())
        ui->baseUrl->setText(config.baseUrl);
    ui->baseUrl->setVisible(AiConfigHelper::isCustomProvider(config.providerName));
    ui->baseUrlLabel->setVisible(AiConfigHelper::isCustomProvider(config.providerName));
    if (ui->model->count() != config.modelList.size() || ui->models->count() != config.modelList.size())
    {
        ui->model->clear();
        ui->model->addItems(config.modelList);
        ui->models->clear();
        ui->models->addItems(config.modelList);
    }
    else
    {
        for (int i = 0; i < config.modelList.size(); ++i)
        {
            ui->model->setItemText(i, config.modelList[i]);
            ui->models->item(i)->setText(config.modelList[i]);
        }
    }
    if (ui->model->currentText() != config.modelName)
        ui->model->setCurrentText(config.modelName);
    const auto matching = ui->models->findItems(config.modelName, Qt::MatchExactly);
    if (!matching.isEmpty())
        ui->models->setCurrentItem(matching.first());
    else
        ui->models->clearSelection();
    ui->statusLabel->setText(!config.apiKey.isEmpty() && !config.modelName.isEmpty() && (!AiConfigHelper::isCustomProvider(config.providerName) || !config.baseUrl.isEmpty()) ? "已配置" : "请填写 API Key 并选择或输入模型");
    m_loading = false;
}
