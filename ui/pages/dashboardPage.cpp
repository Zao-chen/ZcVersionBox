#include "dashboardPage.h"
#include "ui/components/presentation.h"
#include "ui_dashboardPage.h"
#include <QDesktopServices>
#include <QSignalBlocker>
#include <QUrl>
#include <oclero/qlementine/widgets/Expander.hpp>
#include <oclero/qlementine/widgets/Switch.hpp>

DashboardPage::DashboardPage(BackupService *service, QWidget *parent) : QWidget(parent), ui(new Ui::DashboardPage), m_service(service)
{
    ui->setupUi(this);
    m_remoteSwitch = new oclero::qlementine::Switch(this);
    m_remoteSwitch->setObjectName("remoteSwitch");
    m_remoteSwitch->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_remoteSwitch->setFocusPolicy(Qt::StrongFocus);
    m_remoteSwitch->setAccessibleName("云端同步");
    ui->remoteHeaderLayout->addWidget(m_remoteSwitch);
    m_expander = new oclero::qlementine::Expander(this);
    m_expander->setContent(ui->remoteContent);
    ui->remoteLayout->addWidget(m_expander);
    connect(ui->expandButton, &QPushButton::clicked, m_expander, &oclero::qlementine::Expander::toggleExpanded);
    connect(m_expander, &oclero::qlementine::Expander::expandedChanged, this, [this]
            {
                ui->expandButton->setText(m_expander->expanded() ? "收起配置" : "展开配置");
            });
    connect(m_remoteSwitch, &QAbstractButton::toggled, this, &DashboardPage::remoteToggled);
    connect(ui->refreshButton, &QPushButton::clicked, this, &DashboardPage::refresh);
    connect(ui->historyButton, &QPushButton::clicked, this, [this]
            {
                emit navigate({PageId::History, m_id});
            });
    connect(ui->remoteUrl, &QLineEdit::editingFinished, this, [this]
            {
                if (!ui->remoteUrl->isModified())
                    return;
                const auto result = m_service->setRemote(m_id, ui->remoteUrl->text());
                ui->remoteUrl->setModified(false);
                emit notification(result);
                refresh();
            });
    connect(ui->openRemoteButton, &QPushButton::clicked, this, [this]
            {
                if (!QDesktopServices::openUrl(QUrl(ui->remoteUrl->text())))
                    emit notification(OperationResult::fail("打开失败", "无法打开云端地址，请检查链接格式"));
            });
    connect(ui->pullButton, &QPushButton::clicked, this, [this]
            {
                emit notification(m_service->synchronize(m_id, false));
            });
    connect(ui->pushButton, &QPushButton::clicked, this, [this]
            {
                emit notification(m_service->synchronize(m_id, true));
            });
    connect(ui->removeButton, &QPushButton::clicked, this, [this]
            {
                const auto id = m_id;
                const auto generation = m_service->repositoryGeneration(id);
                if (confirmAction(this, "确定要删除这个备份吗？\n\n此操作会删除本地备份仓库和所有历史版本记录，但不会删除源文件。\n\n此操作不可撤销！", "确认删除"))
                    emit notification(generation == m_service->repositoryGeneration(id) ? m_service->removeBackup(id)
                                                                                        : OperationResult::warn("操作已取消", "备份对象已变化，请重新打开此页面后再试"));
            });
    connect(ui->rebuildButton, &QPushButton::clicked, this, [this]
            {
                const auto id = m_id;
                const auto generation = m_service->repositoryGeneration(id);
                if (confirmAction(this, "确定要重建此仓库吗？\n\n删除所有历史版本记录，仅保留当前快照。\n如已配置云端地址，将强制覆盖云端仓库。\n\n此操作不可撤销！", "确认重建"))
                    emit notification(generation == m_service->repositoryGeneration(id) ? m_service->rebuild(id)
                                                                                        : OperationResult::warn("操作已取消", "备份对象已变化，请重新打开此页面后再试"));
            });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id)
            {
                if (id == m_id)
                    refresh();
            });
}
DashboardPage::~DashboardPage() = default;
void DashboardPage::setBackup(const QString &id)
{
    if (id != m_id)
    {
        m_expander->setExpanded(false);
        // A focused editor can survive back/forward navigation on the same page.
        ui->remoteUrl->setModified(false);
        BackupStats stats;
        m_service->statistics(id, stats);
        ui->remoteUrl->setText(stats.remoteUrl);
    }
    m_id = id;
    refresh();
}
void DashboardPage::refresh()
{
    if (m_id.isEmpty())
        return;
    BackupStats stats;
    auto result = m_service->statistics(m_id, stats);
    setEnabled(result.success);
    if (!result.success)
        return;
    const auto source = m_service->sourcePath(m_id);
    ui->titleLabel->setText(QFileInfo(source).fileName());
    ui->titleLabel->setToolTip(source);
    const auto updatePath = [](QLineEdit *field, const QString &path)
    {
        if (field->text() != path)
        {
            field->setText(path);
            field->setCursorPosition(0);
        }
        field->setToolTip(path);
    };
    updatePath(ui->sourcePathEdit, source);
    updatePath(ui->repositoryPathEdit, m_service->repoPath(m_id));
    ui->versionValue->setText(QString::number(stats.versionCount));
    ui->fileValue->setText(QString::number(stats.fileCount));
    ui->sizeValue->setText(formatBytes(stats.fileSize));
    ui->cacheValue->setText(formatBytes(stats.cacheSize));
    ui->stateValue->setText(stats.sourceState);
    QSignalBlocker blocker(m_remoteSwitch);
    m_remoteSwitch->setChecked(!stats.remoteUrl.isEmpty());
    if (!ui->remoteUrl->hasFocus())
    {
        ui->remoteUrl->setText(stats.remoteUrl);
        ui->remoteUrl->setModified(false);
    }
    ui->pullButton->setEnabled(!stats.remoteUrl.isEmpty());
    ui->pushButton->setEnabled(!stats.remoteUrl.isEmpty());
}
void DashboardPage::remoteToggled(bool checked)
{
    if (!checked)
    {
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
