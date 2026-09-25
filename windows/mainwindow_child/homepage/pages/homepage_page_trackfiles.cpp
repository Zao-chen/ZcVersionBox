#include "homepage_page_trackfiles.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
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
    const QPointer<BackupUiActions> guard(this);
    if (!confirmAction(m_owner, QString("确定要删除“%1”的备份吗？\n\n此操作会删除本地备份仓库和所有历史版本记录，但不会删除源文件。\n\n此操作不可撤销！").arg(name), "确认删除") || !guard)
        return;
    if (generation != m_service->repositoryGeneration(id))
        emit notification(OperationResult::warn("操作已取消", "备份对象已变化，请重新打开此页面后再试"));
    else
        m_service->removeBackup(id, this, [this](const OperationResult &result)
                                { emit notification(result); });
}
void BackupUiActions::rebuild(const QString &id)
{
    const auto generation = m_service->repositoryGeneration(id);
    if (!m_service->contains(id))
        return;
    const auto name = QFileInfo(m_service->sourcePath(id)).fileName();
    const QPointer<BackupUiActions> guard(this);
    if (!confirmAction(m_owner, QString("确定要重建“%1”的仓库吗？\n\n删除所有历史版本记录，仅保留当前快照。\n如已配置云端地址，将覆盖已确认的云端版本；云端有新提交时操作会失败。\n\n此操作不可撤销！").arg(name), "确认重建") || !guard)
        return;
    if (generation != m_service->repositoryGeneration(id))
        emit notification(OperationResult::warn("操作已取消", "备份对象已变化，请重新打开此页面后再试"));
    else
        m_service->rebuild(id, this, [this](const OperationResult &result)
                           { emit notification(result); });
}
void BackupUiActions::addLocal(bool directory)
{
    const auto path = directory ? QFileDialog::getExistingDirectory(m_owner, "选择文件夹", QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks)
                                : QFileDialog::getOpenFileName(m_owner, "选择文件", QDir::homePath(), "All Files (*.*)");
    if (!path.isEmpty())
        m_service->addLocal(path, this, [this](const OperationResult &result)
                            { emit notification(result); });
}
void BackupUiActions::importRemote()
{
    auto *dialog = new QDialog(m_owner);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("从云端导入备份");
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    layout->addWidget(new QLabel("云端仓库地址", dialog));
    auto *url = new QLineEdit(dialog);
    url->setAccessibleName("云端仓库地址");
    url->setPlaceholderText("https://github.com/user/repo.git");
    layout->addWidget(url);
    auto *status = new QLabel(dialog);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("读取仓库");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    auto *check = buttons->addButton("检查链接", QDialogButtonBox::ActionRole);
    check->setFlat(true);
    layout->addWidget(buttons);
    connect(check, &QPushButton::clicked, dialog, [this, dialog, check, status, url]
            {
        const auto address = url->text();
        check->setEnabled(false);
        status->setText("正在检查…");
        m_service->checkRemote(address, dialog, [check, status, url, address](const OperationResult &result)
        {
            check->setEnabled(true);
            if (url->text() == address) status->setText(result.title + "：" + result.message);
        }); });
    connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, buttons, check, status, url]
            {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        check->setEnabled(false);
        url->setReadOnly(true);
        status->setText("正在读取仓库…");
        const QPointer<QDialog> guard(dialog);
        const auto task = m_service->prepareImport(url->text(), this, [this, guard, buttons, check, status, url](const BackupResult<PreparedImport> &reply)
        {
            if (!guard)
            {
                if (reply.result.success) m_service->cancelImport(reply.value.sessionId);
                return;
            }
            if (!reply.result.success)
            {
                buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
                check->setEnabled(true);
                url->setReadOnly(false);
                status->setText(reply.result.title + "：" + reply.result.message);
                return;
            }
            guard->accept();
            chooseImport(reply.value);
        });
        connect(dialog, &QDialog::rejected, m_service, [service = m_service, task] { service->cancel(task); }); });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->resize(520, dialog->sizeHint().height());
    dialog->open();
}
void BackupUiActions::chooseImport(const PreparedImport &prepared)
{
    QDialog selection(m_owner);
    selection.setWindowTitle("选择版本控制内容");
    auto *layout = new QVBoxLayout(&selection);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->addWidget(new QLabel("选择整个仓库、一个文件或一个子目录。", &selection));
    auto *entries = new QComboBox(&selection);
    entries->setObjectName("importEntryCombo");
    entries->setAccessibleName("导入内容");
    for (const auto &entry : prepared.entries)
    {
        entries->addItem(entry.path == "." ? "整个仓库" : entry.path + (entry.directory ? "/" : ""));
        if (entry.path == prepared.suggestedPath)
            entries->setCurrentIndex(entries->count() - 1);
    }
    layout->addWidget(entries);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &selection);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &selection, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &selection, &QDialog::reject);
    selection.resize(520, selection.sizeHint().height());
    const QPointer<BackupUiActions> guard(this);
    if (selection.exec() != QDialog::Accepted || !guard)
    {
        if (guard)
            m_service->cancelImport(prepared.sessionId);
        return;
    }
    const auto entry = prepared.entries.value(entries->currentIndex());
    QString target;
    const auto defaultName = entry.path == "." ? QStringLiteral("导入内容") : QFileInfo(entry.path).fileName();
    if (!entry.directory)
        target = QFileDialog::getSaveFileName(m_owner, "选择追踪文件位置", QDir::home().filePath(defaultName), "All Files (*.*)", nullptr, QFileDialog::DontConfirmOverwrite);
    else
    {
        const auto folder = QFileDialog::getExistingDirectory(m_owner, "选择追踪文件夹的父目录", QDir::homePath(), QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!folder.isEmpty() && guard)
        {
            bool accepted = false;
            const auto name = QInputDialog::getText(m_owner, "追踪文件夹名称", "本地文件夹名称", QLineEdit::Normal, defaultName, &accepted).trimmed();
            if (accepted && !name.isEmpty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\\'))
                target = QDir(folder).filePath(name);
        }
    }
    if (!guard)
        return;
    if (target.isEmpty())
    {
        m_service->cancelImport(prepared.sessionId);
        return;
    }
    const bool replace = QFileInfo::exists(target);
    if (replace && (!confirmAction(m_owner, "导入将替换目标位置的当前内容：\n" + QDir::toNativeSeparators(target), "确认导入") || !guard))
    {
        if (guard)
            m_service->cancelImport(prepared.sessionId);
        return;
    }
    m_service->finishImport(prepared.sessionId, entry.path, target, replace, this, [this, session = prepared.sessionId](const OperationResult &result)
                            {
        if (!result.success) m_service->cancelImport(session);
        emit notification(result); });
}
