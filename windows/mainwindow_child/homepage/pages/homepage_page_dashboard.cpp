#include "homepage_page_dashboard.h"
#include "ui_homepage_page_dashboard.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <oclero/qlementine/widgets/Expander.hpp>
#include <oclero/qlementine/widgets/Switch.hpp>

HomePageDashboardPage::HomePageDashboardPage(BackupService *service, QWidget *parent) : QWidget(parent), ui(new Ui::HomePageDashboardPage), m_service(service)
{
    ui->setupUi(this);
    for (auto *label : {ui->sourceLabel, ui->repositoryLabel, ui->stateValue, ui->versionCaption, ui->fileCaption, ui->sizeCaption, ui->cacheCaption, ui->remoteHint})
        UiStyle::text(label, UiStyle::FontRole::Caption, true);
    for (auto *value : {ui->versionValue, ui->fileValue, ui->sizeValue, ui->cacheValue})
        UiStyle::text(value, UiStyle::FontRole::Object);
    UiStyle::text(ui->sourcePathEdit, UiStyle::FontRole::Body);
    UiStyle::text(ui->repositoryPathEdit, UiStyle::FontRole::Body);
    ui->sourcePathEdit->setAccessibleName("源位置");
    ui->repositoryPathEdit->setAccessibleName("备份仓库位置");
    ui->remoteUrl->setAccessibleName("云端仓库地址");
    ui->remoteUrlLabel->setBuddy(ui->remoteUrl);
    m_refresh = UiStyle::action(this, "refreshOverviewAction", "刷新概览", "refresh");
    m_refresh->setProperty("iconOnly", true);
    connect(m_refresh, &QAction::triggered, this, &HomePageDashboardPage::refresh);
    for (auto *button : {ui->copySourceButton, ui->copyRepositoryButton})
        button->setIcon(UiStyle::icon("copy"));
    for (auto *button : {ui->openSourceButton, ui->openRepositoryButton})
        button->setIcon(UiStyle::icon("external"));
    ui->expandButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    ui->expandButton->setIcon(UiStyle::icon("chevron_right"));
    connect(ui->copySourceButton, &QToolButton::clicked, this, [this]
            { QApplication::clipboard()->setText(ui->sourcePathEdit->text()); });
    connect(ui->copyRepositoryButton, &QToolButton::clicked, this, [this]
            { QApplication::clipboard()->setText(ui->repositoryPathEdit->text()); });
    connect(ui->openSourceButton, &QToolButton::clicked, this, [this]
            { openLocalPath(this, m_service->sourcePath(m_id)); });
    connect(ui->openRepositoryButton, &QToolButton::clicked, this, [this]
            { openLocalPath(this, m_service->repoPath(m_id)); });
    m_remoteSwitch = new oclero::qlementine::Switch(this);
    m_remoteSwitch->setObjectName("remoteSwitch");
    m_remoteSwitch->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_remoteSwitch->setFocusPolicy(Qt::StrongFocus);
    m_remoteSwitch->setAccessibleName("云端同步");
    ui->remoteHeaderLayout->addWidget(m_remoteSwitch);
    ui->remoteLayout->removeWidget(ui->remoteContent);
    m_expander = new oclero::qlementine::Expander(this);
    m_expander->setObjectName("remoteExpander");
    m_expander->setContent(ui->remoteContent);
    ui->remoteLayout->addWidget(m_expander);
    connect(ui->expandButton, &QToolButton::clicked, m_expander, &oclero::qlementine::Expander::toggleExpanded);
    connect(m_expander, &oclero::qlementine::Expander::expandedChanged, this, [this]
            {
        ui->expandButton->setText(m_expander->expanded() ? "收起配置" : "展开配置");
        ui->expandButton->setIcon(UiStyle::icon(m_expander->expanded() ? "arrow_down" : "chevron_right"));
        ui->expandButton->setAccessibleDescription(m_expander->expanded() ? "已展开" : "已折叠"); });
    connect(m_remoteSwitch, &QAbstractButton::toggled, this, &HomePageDashboardPage::remoteToggled);
    connect(ui->remoteUrl, &QLineEdit::editingFinished, this, [this]
            {
        if (!ui->remoteUrl->isModified())
            return;
        ui->remoteUrl->setModified(false);
        emit notification(m_service->setRemote(m_id, ui->remoteUrl->text()));
        refresh(); });
    connect(ui->openRemoteButton, &QPushButton::clicked, this, [this]
            {
        if (!QDesktopServices::openUrl(QUrl(ui->remoteUrl->text())))
            emit notification(OperationResult::fail("打开失败", "无法打开云端地址，请检查链接格式")); });
    connect(ui->pullButton, &QPushButton::clicked, this, [this]
            { emit notification(m_service->synchronize(m_id, false)); });
    connect(ui->pushButton, &QPushButton::clicked, this, [this]
            { emit notification(m_service->synchronize(m_id, true)); });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id)
            {
        if (id == m_id && isVisible())
            refresh(); });
    connect(service, &BackupService::repositoryInvalidated, this, [this](const QString &id)
            {
        m_states.remove(id);
        if (id == m_id)
        {
            ui->remoteUrl->setModified(false);
            m_expander->setExpanded(false);
        } });
}
HomePageDashboardPage::~HomePageDashboardPage() = default;
QList<QAction *> HomePageDashboardPage::toolbarActions() const { return {m_refresh}; }
void HomePageDashboardPage::rememberState()
{
    if (!m_id.isEmpty() && m_repositoryGeneration == m_service->repositoryGeneration(m_id))
        m_states[m_id] = {m_repositoryGeneration, ui->scroll->verticalScrollBar()->value(), m_expander->expanded()};
}
void HomePageDashboardPage::deactivate() { rememberState(); }
void HomePageDashboardPage::setBackup(const QString &id)
{
    rememberState();
    const bool changed = id != m_id || m_repositoryGeneration != m_service->repositoryGeneration(id);
    if (changed)
    {
        // A focused editor can survive navigation to another object.
        ui->remoteUrl->setModified(false);
        ui->remoteUrl->clear();
    }
    m_id = id;
    m_repositoryGeneration = m_service->repositoryGeneration(id);
    refresh();
    const auto state = m_states.value(id);
    m_expander->setExpanded(state.generation == m_repositoryGeneration && state.expanded);
    QTimer::singleShot(0, this, [this, id, state]
                       {
        if (m_id == id)
            ui->scroll->verticalScrollBar()->setValue(state.generation == m_repositoryGeneration ? state.scroll : 0); });
}
void HomePageDashboardPage::refresh()
{
    if (m_id.isEmpty())
        return;
    BackupStats stats;
    const auto result = m_service->statistics(m_id, stats);
    setEnabled(result.success);
    if (!result.success)
        return;
    const auto updatePath = [](QLineEdit *field, const QString &path)
    {
        const auto native = QDir::toNativeSeparators(path);
        if (field->text() != native)
        {
            field->setText(native);
            field->setCursorPosition(0);
        }
        field->setToolTip(native);
    };
    updatePath(ui->sourcePathEdit, m_service->sourcePath(m_id));
    updatePath(ui->repositoryPathEdit, m_service->repoPath(m_id));
    ui->versionValue->setText(QString::number(stats.versionCount));
    ui->fileValue->setText(QString::number(stats.fileCount));
    ui->sizeValue->setText(formatBytes(stats.fileSize));
    ui->cacheValue->setText(formatBytes(stats.cacheSize));
    ui->stateValue->setText(stats.sourceState);
    const QSignalBlocker blocker(m_remoteSwitch);
    m_remoteSwitch->setChecked(!stats.remoteUrl.isEmpty());
    if (!ui->remoteUrl->hasFocus() || !ui->remoteUrl->isModified())
    {
        ui->remoteUrl->setText(stats.remoteUrl);
        ui->remoteUrl->setModified(false);
    }
    ui->pullButton->setEnabled(!stats.remoteUrl.isEmpty());
    ui->pushButton->setEnabled(!stats.remoteUrl.isEmpty());
    ui->openRemoteButton->setEnabled(!stats.remoteUrl.isEmpty());
}
void HomePageDashboardPage::remoteToggled(bool checked)
{
    if (!checked)
    {
        ui->remoteUrl->setModified(false);
        emit notification(m_service->removeRemote(m_id));
        refresh();
        m_expander->setExpanded(false);
    }
    else
    {
        m_expander->setExpanded(true);
        ui->remoteUrl->setFocus();
    }
}
void HomePageDashboardPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto margin = width() < 640 ? 16 : 24;
    ui->readingLayout->setContentsMargins(margin, 24, margin, 24);
}
