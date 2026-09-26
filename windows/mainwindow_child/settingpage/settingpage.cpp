#include "settingpage.h"
#include "ui_settingpage.h"
#include "ui_settingpage_page_ai.h"
#include "windows/mainwindow_presentation.h"
#include <QCompleter>
#include <QHideEvent>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QSignalBlocker>
#include <oclero/qlementine/widgets/LoadingSpinner.hpp>
#include <oclero/qlementine/widgets/SegmentedControl.hpp>
#include <oclero/qlementine/widgets/Switch.hpp>

SettingPage::SettingPage(SettingsService *settings, ThemeController *theme, QWidget *parent)
    : QWidget(parent), ui(new Ui::SettingPage), m_theme(theme)
{
    ui->setupUi(this);
    UiStyle::text(ui->pageTitle, UiStyle::FontRole::Page);
    UiStyle::text(ui->appearanceSectionTitle, UiStyle::FontRole::Section);
    UiStyle::text(ui->systemSectionTitle, UiStyle::FontRole::Section);
    UiStyle::text(ui->themeDescription, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->rightClickDescription, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->autoStartDescription, UiStyle::FontRole::Caption, true);
#ifdef Q_OS_LINUX
    ui->rightClickDescription->setText("Nautilus 文件管理器：右键 → 脚本 → 添加到 ZcVersionBox");
    ui->autoStartDescription->setText("登录 Linux 桌面后自动启动");
#endif
    auto *themeMode = new oclero::qlementine::SegmentedControl(this);
    themeMode->setObjectName("themeModeControl");
    themeMode->setAccessibleName("主题");
    themeMode->addItem("系统", {}, {}, static_cast<int>(ThemeMode::System));
    themeMode->addItem("浅色", {}, {}, static_cast<int>(ThemeMode::Light));
    themeMode->addItem("深色", {}, {}, static_cast<int>(ThemeMode::Dark));
    ui->themeLayout->addWidget(themeMode);
    ui->themeLayout->setAlignment(themeMode, Qt::AlignVCenter);
    const auto syncThemeMode = [=]
    {
        const QSignalBlocker blocker(themeMode);
        themeMode->setCurrentIndex(themeMode->findItemIndex(static_cast<int>(m_theme->mode())));
    };
    syncThemeMode();
    connect(m_theme, &ThemeController::changed, this, syncThemeMode);
    connect(themeMode, &oclero::qlementine::AbstractItemListWidget::currentIndexChanged, this, [=]
            {
        const auto data = themeMode->currentData();
        if (data.isValid())
            m_theme->setMode(static_cast<ThemeMode>(data.toInt())); });
    auto *rightClick = new oclero::qlementine::Switch(this);
    rightClick->setObjectName("rightClickSwitch");
    rightClick->setAccessibleName("右键菜单快捷入口");
    auto *autoStart = new oclero::qlementine::Switch(this);
    autoStart->setObjectName("autoStartSwitch");
    autoStart->setAccessibleName("开机自动启动");
    for (auto *toggle : {rightClick, autoStart})
    {
        toggle->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        toggle->setFocusPolicy(Qt::TabFocus);
    }
    ui->rightClickLayout->addWidget(rightClick);
    ui->rightClickLayout->setStretch(0, 1);
    ui->rightClickLayout->setAlignment(rightClick, Qt::AlignVCenter);
    ui->autoStartLayout->addWidget(autoStart);
    ui->autoStartLayout->setStretch(0, 1);
    ui->autoStartLayout->setAlignment(autoStart, Qt::AlignVCenter);
    const auto reload = [=]
    {
        const QSignalBlocker b1(rightClick), b2(autoStart);
        rightClick->setChecked(settings->value("RightClickMenu", false).toBool());
        autoStart->setChecked(settings->value("AutoStart", false).toBool());
    };
    reload();
    connect(settings, &SettingsService::changed, this, reload);
    connect(rightClick, &QAbstractButton::toggled, this, [=](bool checked)
            {
        emit notification(settings->setSystemOption("RightClickMenu", checked));
        reload(); });
    connect(autoStart, &QAbstractButton::toggled, this, [=](bool checked)
            {
        emit notification(settings->setSystemOption("AutoStart", checked));
        reload(); });
}
SettingPage::~SettingPage() = default;
void SettingPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool compact = width() < 640;
    const auto margin = compact ? 16 : 24;
    ui->readingLayout->setContentsMargins(margin, compact ? 24 : 64, margin, 32);
}
SettingPageAiPage::SettingPageAiPage(SettingsService *settings, QWidget *parent) : QWidget(parent), ui(new Ui::SettingPageAiPage), m_settings(settings)
{
    ui->setupUi(this);
    UiStyle::text(ui->pageTitle, UiStyle::FontRole::Page);
    for (auto *label : {ui->automationSectionTitle, ui->serviceSectionTitle})
        UiStyle::text(label, UiStyle::FontRole::Section);
    for (auto *label : {ui->enabledDescription, ui->providerDescription, ui->apiKeyDescription, ui->baseUrlDescription, ui->modelDescription, ui->statusLabel})
        UiStyle::text(label, UiStyle::FontRole::Caption, true);
    ui->provider->addItems({"OpenAI", "DeepSeek", "Custom"});
    ui->providerLabel->setBuddy(ui->provider);
    ui->modelLabel->setBuddy(ui->model);
    ui->apiKeyLabel->setBuddy(ui->apiKey);
    ui->baseUrlLabel->setBuddy(ui->baseUrl);
    ui->apiKey->setAccessibleName("API Key");
    ui->baseUrl->setAccessibleName("自定义服务地址");
    ui->model->setInsertPolicy(QComboBox::NoInsert);
    ui->model->setMaxVisibleItems(10);
    ui->model->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    ui->model->completer()->setFilterMode(Qt::MatchContains);
    ui->model->completer()->setCompletionMode(QCompleter::PopupCompletion);
    ui->model->lineEdit()->setPlaceholderText("选择或输入模型名称");
    m_enabled = new oclero::qlementine::Switch(this);
    m_enabled->setObjectName("aiEnabledSwitch");
    m_enabled->setAccessibleName("AI 自动提交说明");
    m_enabled->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_enabled->setFocusPolicy(Qt::TabFocus);
    ui->enabledLayout->addWidget(m_enabled);
    ui->enabledLayout->setStretch(0, 1);
    ui->enabledLayout->setAlignment(m_enabled, Qt::AlignVCenter);
    m_spinner = new oclero::qlementine::LoadingSpinner(this);
    m_spinner->setObjectName("modelsSpinner");
    m_spinner->setAccessibleName("正在获取模型");
    m_spinner->setFixedSize(16, 16);
    ui->modelStatusLayout->insertWidget(0, m_spinner);
    ui->modelStatusLayout->setStretch(1, 1);
    ui->modelStatusLayout->setAlignment(m_spinner, Qt::AlignTop);
    connect(m_enabled, &QAbstractButton::toggled, this, [this](bool enabled)
            {
        if (!m_loading)
            m_settings->setAiEnabled(enabled); });
    connect(ui->provider, &QComboBox::currentTextChanged, this, [this](const QString &name)
            {
        if (!m_loading)
        {
            m_fetchError.clear();
            m_settings->selectProvider(name);
            emit navigate({PageId::AiSettings, {}, {}, name});
        } });
    connect(ui->apiKey, &QLineEdit::textChanged, this, [this](const QString &text)
            {
        if (!m_loading)
        {
            m_fetchError.clear();
            m_settings->saveField("ApiKey", text);
        } });
    connect(ui->baseUrl, &QLineEdit::textChanged, this, [this](const QString &text)
            {
        if (!m_loading)
        {
            m_fetchError.clear();
            m_settings->saveField("BaseUrl", text);
        } });
    connect(ui->model, &QComboBox::currentTextChanged, this, [this](const QString &text)
            {
        if (!m_loading)
            m_settings->saveField("Model", text); });
    connect(ui->fetchButton, &QPushButton::clicked, settings, &SettingsService::fetchModels);
    connect(settings, &SettingsService::changed, this, &SettingPageAiPage::refresh);
    connect(settings, &SettingsService::fetchingChanged, this, [this](bool fetching)
            {
        if (fetching)
            m_fetchError.clear();
        updateLoadingState(); });
    connect(settings, &SettingsService::notification, this, [this](const OperationResult &result)
            {
        if (result.title == "模型获取失败")
        {
            m_fetchError = result.message;
            updateLoadingState();
        } });
    refresh();
}
SettingPageAiPage::~SettingPageAiPage() = default;
void SettingPageAiPage::refresh()
{
    const QScopedValueRollback<bool> loading(m_loading, true);
    const auto config = m_settings->config(m_settings->provider());
    const bool providerChanged = config.providerName != m_displayedProvider;
    if (providerChanged)
        m_fetchError.clear();
    m_displayedProvider = config.providerName;
    const QSignalBlocker toggleBlocker(m_enabled);
    m_enabled->setChecked(m_settings->value("AI/Enabled", m_settings->value("AI/AutoCommitMessage", false)).toBool());
    ui->provider->setCurrentText(config.providerName);
    if (providerChanged || !ui->apiKey->hasFocus())
        ui->apiKey->setText(config.apiKey);
    if (providerChanged || !ui->baseUrl->hasFocus())
        ui->baseUrl->setText(config.baseUrl);
    const bool custom = AiConfigHelper::isCustomProvider(config.providerName);
    ui->baseUrlRow->setVisible(custom);
    ui->baseUrlSeparator->setVisible(custom);
    QStringList currentList;
    for (int i = 0; i < ui->model->count(); ++i)
        currentList.append(ui->model->itemText(i));
    if (currentList != config.modelList)
    {
        ui->model->clear();
        ui->model->addItems(config.modelList);
        ui->model->setCurrentText(config.modelName);
    }
    else if (providerChanged || !ui->model->lineEdit()->hasFocus())
        ui->model->setCurrentText(config.modelName);
    updateLoadingState();
}
void SettingPageAiPage::updateLoadingState()
{
    const bool fetching = m_settings->isFetching();
    m_spinner->setSpinning(fetching && isVisible());
    m_spinner->setVisible(fetching);
    ui->fetchButton->setEnabled(!fetching);
    const auto config = m_settings->config(m_settings->provider());
    const bool configured = !config.apiKey.isEmpty() && !config.modelName.isEmpty() && (!AiConfigHelper::isCustomProvider(config.providerName) || !config.baseUrl.isEmpty());
    const auto status = fetching ? QString("正在获取模型…") : !m_fetchError.isEmpty() ? m_fetchError
                                                          : configured                ? QString("已配置")
                                                                                      : QString("请配置 API Key 和模型。");
    ui->statusLabel->setText(status);
    ui->statusLabel->setToolTip(status);
}
void SettingPageAiPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateLoadingState();
}
void SettingPageAiPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_spinner->setSpinning(false);
}
void SettingPageAiPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const bool compact = width() < 640;
    const auto margin = compact ? 16 : 24;
    ui->readingLayout->setContentsMargins(margin, compact ? 24 : 64, margin, 32);
    const QList<QPair<QBoxLayout *, QWidget *>> rows{
        {ui->providerLayout, ui->provider}, {ui->apiKeyLayout, ui->apiKey}, {ui->baseUrlLayout, ui->baseUrl}, {ui->modelLayout, ui->modelFields}};
    for (const auto &[layout, field] : rows)
    {
        layout->setDirection(compact ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
        layout->setSpacing(compact ? 8 : 24);
        layout->setStretch(0, compact ? 0 : 1);
        layout->setAlignment(layout->itemAt(0)->layout(), compact ? Qt::Alignment{} : Qt::AlignVCenter);
        field->setMinimumWidth(compact ? 0 : 320);
        field->setMaximumWidth(compact ? QWIDGETSIZE_MAX : 320);
        layout->setAlignment(field, compact ? Qt::Alignment{} : Qt::AlignVCenter);
    }
}
