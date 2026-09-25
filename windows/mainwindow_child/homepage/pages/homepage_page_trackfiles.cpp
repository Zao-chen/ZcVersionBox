#include "homepage_page_trackfiles.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QVBoxLayout>

BackupUiActions::BackupUiActions(BackupService *service, QWidget *owner) : QObject(owner), m_service(service), m_owner(owner)
{
    m_addMenu = new QMenu(owner);
    m_addMenu->setObjectName("addBackupMenu");
    auto *file = UiStyle::action(this, "addFileAction", "添加文件…", "file");
    auto *folder = UiStyle::action(this, "addFolderAction", "添加文件夹…", "folder");
    auto *cloud = UiStyle::action(this, "importBackupAction", "从云端导入…", "cloud");
    m_addMenu->addActions({file, folder, cloud});
    m_add = UiStyle::action(this, "addBackupAction", "添加备份", "add");
    m_add->setMenu(m_addMenu);
    m_add->setProperty("primary", true);
    connect(file, &QAction::triggered, this, [this]
            { addLocal(false); });
    connect(folder, &QAction::triggered, this, [this]
            { addLocal(true); });
    connect(cloud, &QAction::triggered, this, &BackupUiActions::importRemote);
    m_open = UiStyle::action(this, "openSourceAction", "打开源文件", "external");
    m_overview = UiStyle::action(this, "overviewAction", "查看概览", "info");
    m_copy = UiStyle::action(this, "copyPathAction", "复制路径", "copy");
    m_remove = UiStyle::action(this, "removeBackupAction", "删除备份…");
    m_rebuild = UiStyle::action(this, "rebuildBackupAction", "重建仓库…");
    connect(m_open, &QAction::triggered, this, [this]
            {
        if (m_service->contains(target()))
            openLocalPath(m_owner, m_service->sourcePath(target())); });
    connect(m_copy, &QAction::triggered, this, [this]
            {
        if (m_service->contains(target()))
            QApplication::clipboard()->setText(QDir::toNativeSeparators(m_service->sourcePath(target()))); });
    connect(m_overview, &QAction::triggered, this, [this]
            { emit navigate({PageId::Dashboard, target()}); });
    connect(m_remove, &QAction::triggered, this, [this]
            { remove(target()); });
    connect(m_rebuild, &QAction::triggered, this, [this]
            { rebuild(target()); });
}
void BackupUiActions::refreshIcons()
{
    for (auto *action : findChildren<QAction *>())
        if (!action->property("iconName").toString().isEmpty())
            action->setIcon(UiStyle::icon(action->property("iconName").toString()));
}
void BackupUiActions::showObjectMenu(const QString &id, const QPoint &position, bool maintenance)
{
    if (!m_service->contains(id))
        return;
    QScopedValueRollback<QString> context(m_menuId, id);
    QMenu menu(m_owner);
    menu.setObjectName("backupObjectMenu");
    menu.addActions({m_open, m_overview, m_copy});
    if (maintenance)
    {
        menu.addSeparator();
        menu.addActions({m_rebuild, m_remove});
    }
    menu.exec(position);
}
void BackupUiActions::remove(const QString &id)
{
    const auto generation = m_service->repositoryGeneration(id);
    if (!m_service->contains(id))
        return;
    const auto name = QFileInfo(m_service->sourcePath(id)).fileName();
    if (confirmAction(m_owner, QString("确定要删除“%1”的备份吗？\n\n此操作会删除本地备份仓库和所有历史版本记录，但不会删除源文件。\n\n此操作不可撤销！").arg(name), "确认删除"))
        emit notification(generation == m_service->repositoryGeneration(id) ? m_service->removeBackup(id)
                                                                            : OperationResult::warn("操作已取消", "备份对象已变化，请重新打开此页面后再试"));
}
void BackupUiActions::rebuild(const QString &id)
{
    const auto generation = m_service->repositoryGeneration(id);
    if (!m_service->contains(id))
        return;
    const auto name = QFileInfo(m_service->sourcePath(id)).fileName();
    if (confirmAction(m_owner, QString("确定要重建“%1”的仓库吗？\n\n删除所有历史版本记录，仅保留当前快照。\n如已配置云端地址，将强制覆盖云端仓库。\n\n此操作不可撤销！").arg(name), "确认重建"))
        emit notification(generation == m_service->repositoryGeneration(id) ? m_service->rebuild(id)
                                                                            : OperationResult::warn("操作已取消", "备份对象已变化，请重新打开此页面后再试"));
}
void BackupUiActions::addLocal(bool directory)
{
    const auto path = directory ? QFileDialog::getExistingDirectory(m_owner, "选择文件夹", QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks)
                                : QFileDialog::getOpenFileName(m_owner, "选择文件", QDir::homePath(), "All Files (*.*)");
    if (!path.isEmpty())
        emit notification(m_service->addLocal(path));
}
void BackupUiActions::importRemote()
{
    QDialog dialog(m_owner);
    dialog.setWindowTitle("从云端导入备份");
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    layout->addWidget(new QLabel("云端仓库地址", &dialog));
    auto *url = new QLineEdit(&dialog);
    url->setAccessibleName("云端仓库地址");
    url->setPlaceholderText("https://github.com/user/repo.git");
    layout->addWidget(url);
    auto *status = new QLabel(&dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("导入");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    auto *check = buttons->addButton("检查链接", QDialogButtonBox::ActionRole);
    check->setFlat(true);
    layout->addWidget(buttons);
    connect(check, &QPushButton::clicked, &dialog, [&]
            {
        const auto result = m_service->checkRemote(url->text());
        status->setText(result.title + "：" + result.message); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(520, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto prepared = m_service->prepareImport(url->text());
    if (!prepared.result.success)
    {
        emit notification(prepared.result);
        return;
    }
    QString target;
    if (!prepared.directory)
        target = QFileDialog::getSaveFileName(m_owner, "选择追踪文件位置", QDir::home().filePath(prepared.entryName), "All Files (*.*)");
    else
    {
        const auto folder = QFileDialog::getExistingDirectory(m_owner, "选择追踪文件夹位置", QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!folder.isEmpty())
            target = QDir(folder).filePath(prepared.entryName);
    }
    if (target.isEmpty())
    {
        m_service->cancelImport(prepared.temporaryRepo);
        return;
    }
    const auto result = m_service->finishImport(prepared.temporaryRepo, target);
    if (!result.success)
        m_service->cancelImport(prepared.temporaryRepo);
    emit notification(result);
}
