#include "homepage_page_branches.h"
#include "ui_homepage_page_branches.h"
#include "windows/mainwindow_dialog.h"
#include "windows/mainwindow_presentation.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace
{
constexpr int RefRole = Qt::UserRole + 1;
QDialogButtonBox *dialogButtons(UiDialog::Dialog &dialog)
{
    auto *buttons = dialog.buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    dialog.contentLayout()->addWidget(buttons);
    dialog.bindButtonBox(buttons);
    return buttons;
}
QString changeList(const QVector<DiffFile> &changes)
{
    QStringList lines;
    for (const auto &file : changes.mid(0, 200))
        lines.append((file.status == "A" ? "新增  " : file.status == "D" ? "删除  " : "修改  ") + file.path);
    if (changes.size() > 200) lines.append(QString("另有 %1 项变化").arg(changes.size() - 200));
    return lines.isEmpty() ? QStringLiteral("文件内容相同，只切换方案。") : lines.join('\n');
}
// Cloud branches are shown by their name on the server, without the remote prefix.
QString displayName(const BranchInfo &branch)
{
    return branch.remoteBranch && branch.remoteRef.startsWith("refs/heads/") ? branch.remoteRef.mid(11) : branch.name;
}
QStandardItem *groupItem(const QString &text)
{
    auto *item = new QStandardItem(text);
    item->setFlags(Qt::NoItemFlags);
    auto font = item->font(); font.setBold(true); item->setFont(font);
    return item;
}
}

HomePageBranchesPage::HomePageBranchesPage(BackupService *service, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePageBranchesPage), m_service(service)
{
    ui->setupUi(this);
    UiStyle::text(ui->listHint, UiStyle::FontRole::Caption, true);
    ui->branchList->setModel(&m_listModel);
    ui->branchList->setSpacing(2);
    m_refresh = UiStyle::action(this, "refreshBranchesAction", "刷新", "refresh");
    m_refresh->setProperty("iconOnly", true);
    connect(m_refresh, &QAction::triggered, this, &HomePageBranchesPage::refresh);
    connect(ui->listUse, &QPushButton::clicked, this, [this] { useBranch(selectedBranch()); });
    connect(ui->listCreate, &QPushButton::clicked, this, [this] { createFrom(m_id); });
    connect(ui->listOperations, &QPushButton::clicked, this, &HomePageBranchesPage::showOperations);
    connect(ui->branchList->selectionModel(), &QItemSelectionModel::currentChanged, this, &HomePageBranchesPage::updateActions);
    // Viewing one 方案 reuses the history page's scope filter instead of a separate page.
    connect(ui->branchList, &QListView::activated, this, [this](const QModelIndex &index)
    {
        const auto ref = index.data(RefRole).toString();
        if (ref.isEmpty()) return;
        Route route{PageId::History, m_id}; route.branchRef = ref; emit navigate(route);
    });
    connect(ui->cancelTask, &QPushButton::clicked, this, [this] { if (m_task) m_service->cancel(m_task); });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id)
    {
        if (id == m_id && isVisible() && !m_busy) refresh();
    });
    updateActions();
}
HomePageBranchesPage::~HomePageBranchesPage() = default;
QList<QAction *> HomePageBranchesPage::toolbarActions() const { return {m_refresh}; }
QWidget *HomePageBranchesPage::dialogOwner() const { return const_cast<HomePageBranchesPage *>(this); }
BranchInfo HomePageBranchesPage::selectedBranch() const
{
    const auto ref = ui->branchList->currentIndex().data(RefRole).toString();
    for (const auto &branch : m_snapshot.branches)
        if (!ref.isEmpty() && branch.ref == ref) return branch;
    return {};
}
BranchRequest HomePageBranchesPage::request(const BranchInfo &branch) const
{
    BranchRequest result;
    result.context = m_snapshot.context;
    result.ref = branch.ref;
    result.expectedHead = branch.head;
    result.endpoint = branch.endpoint;
    return result;
}
void HomePageBranchesPage::setBackup(const QString &id)
{
    if (m_id != id)
    {
        ++m_generation;
        m_snapshot = {};
        m_listModel.clear();
    }
    m_id = id;
    refresh();
}
void HomePageBranchesPage::refresh()
{
    if (m_id.isEmpty() || m_busy) return;
    const auto generation = ++m_generation;
    const auto id = m_id;
    m_service->branches(id, this, [this, id, generation](const BackupResult<BranchSnapshot> &reply)
    {
        if (id != m_id || generation != m_generation) return;
        if (!reply.result.success) { emit notification(reply.result); return; }
        m_snapshot = reply.value;
        populateList();
        updateActions();
    });
}
void HomePageBranchesPage::populateList()
{
    const auto previous = ui->branchList->currentIndex().data(RefRole).toString();
    m_listModel.clear();
    QList<QStandardItem *> local, cloud;
    for (const auto &branch : m_snapshot.branches)
    {
        if (isLinkedRemote(branch, m_snapshot.branches)) continue;
        QString status;
        if (branch.remoteBranch) status = "仅在云端，可加入";
        else status = (branch.current ? QStringLiteral("正在使用 · ") : QString()) + (branch.upstream.isEmpty() ? "仅本机" : "已共享");
        auto *item = new QStandardItem(displayName(branch) + "　" + status + (branch.message.isEmpty() ? QString() : "\n" + branch.message));
        item->setData(branch.ref, RefRole);
        item->setData(displayName(branch) + "，" + status, Qt::AccessibleTextRole);
        item->setToolTip("双击查看这个方案的历史版本");
        item->setEditable(false);
        if (branch.current) { auto font = item->font(); font.setBold(true); item->setFont(font); }
        (branch.remoteBranch ? cloud : local).append(item);
    }
    m_listModel.appendRow(groupItem("我的方案"));
    for (auto *item : local) m_listModel.appendRow(item);
    if (!cloud.isEmpty())
    {
        m_listModel.appendRow(groupItem("云端其他方案"));
        for (auto *item : cloud) m_listModel.appendRow(item);
    }
    QModelIndex select;
    for (int row = 0; row < m_listModel.rowCount(); ++row)
    {
        const auto index = m_listModel.index(row, 0);
        const auto ref = index.data(RefRole).toString();
        if (ref.isEmpty()) continue;
        if (ref == previous || (!select.isValid() && previous.isEmpty() && ref == m_snapshot.context.ref)) select = index;
    }
    if (!select.isValid())
        for (int row = 0; row < m_listModel.rowCount() && !select.isValid(); ++row)
            if (!m_listModel.index(row, 0).data(RefRole).toString().isEmpty()) select = m_listModel.index(row, 0);
    ui->branchList->setCurrentIndex(select);
}
void HomePageBranchesPage::complete(const OperationResult &result, bool reload)
{
    m_busy = false;
    m_task = 0;
    emit notification(result);
    updateActions();
    if (result.success && reload) refresh();
}
void HomePageBranchesPage::updateActions()
{
    const auto selected = selectedBranch();
    const bool available = !m_id.isEmpty() && !selected.ref.isEmpty() && m_service->contains(m_id);
    const bool tracking = !m_id.isEmpty() && m_service->contains(m_id) && m_service->syncState(m_id) == BackupSyncState::Tracking;
    const bool editable = available && !m_busy && tracking;
    m_refresh->setEnabled(!m_id.isEmpty() && !m_busy);
    ui->listUse->setEnabled(editable && !selected.current);
    ui->listUse->setText(selected.remoteBranch ? "加入并使用…" : selected.current ? "正在使用" : "使用此方案…");
    ui->listOperations->setEnabled(editable);
    ui->listCreate->setEnabled(!m_busy && tracking);
    const auto reason = tracking ? QString() : QString("当前状态：%1。处理完成后才能管理方案。").arg(m_id.isEmpty() ? QString() : backupStateText(m_service->syncState(m_id)));
    for (auto *button : {ui->listUse, ui->listCreate}) button->setToolTip(reason);
    ui->cancelTask->setVisible(m_busy);
}
void HomePageBranchesPage::useBranch(const BranchInfo &branch)
{
    if (branch.ref.isEmpty()) return;
    if (branch.remoteBranch) createFrom(m_id, branch.head);
    else requestSwitch(m_id, branch.ref);
}
void HomePageBranchesPage::createFrom(const QString &id, const QString &commit)
{
    if (m_busy || !m_service->contains(id)) return;
    const auto context = m_service->branchContext(id);
    const QPointer<HomePageBranchesPage> guard(this);
    const auto selected = selectedBranch();
    const bool tracking = id == m_id && selected.remoteBranch && selected.head == commit;
    const auto upstream = tracking ? selected.ref : QString();
    UiDialog::Dialog dialog(dialogOwner(), "createBranchDialog", tracking ? "加入云端方案" : "新建方案");
    const auto text = tracking ? QString("在本机加入云端方案“%1”。之后的修改会同步回这个云端方案。").arg(displayName(selected))
                    : commit.isEmpty() || commit == context.head ? QStringLiteral("从当前方案的最新版本开始一个新方案。原方案不受影响，随时可以切换回来。")
                    : QStringLiteral("从所选的历史版本开始一个新方案。原方案不受影响，随时可以切换回来。");
    auto *description = new QLabel(text, &dialog);
    description->setWordWrap(true); dialog.contentLayout()->addWidget(description);
    auto *name = new QLineEdit(&dialog); name->setObjectName("branchName"); name->setPlaceholderText("例如：尝试新排版");
    if (tracking) name->setText(displayName(selected));
    dialog.contentLayout()->addWidget(name);
    auto *rule = new QLabel("可以使用中文；不能包含空格以及 ~ ^ : ? * [ \\ 等符号。", &dialog);
    rule->setWordWrap(true); UiStyle::text(rule, UiStyle::FontRole::Caption, true); dialog.contentLayout()->addWidget(rule);
    auto *use = new QCheckBox("创建后立即使用（会先保存当前修改）", &dialog); use->setChecked(true); dialog.contentLayout()->addWidget(use);
    auto *buttons = dialogButtons(dialog); buttons->button(QDialogButtonBox::Ok)->setText(tracking ? "加入方案" : "创建方案");
    if (dialog.exec() != QDialog::Accepted || !guard) return;
    BranchRequest request; request.context = context; request.name = name->text(); request.startCommit = commit.isEmpty() ? context.head : commit;
    request.upstream = upstream;
    const bool switchAfter = use->isChecked();
    m_busy = true; updateActions();
    m_task = m_service->createBranch(request, this, [this, id, branch = request.name, switchAfter](const OperationResult &result)
    {
        complete(result);
        if (result.success && switchAfter) requestSwitch(id, "refs/heads/" + branch);
    });
}
void HomePageBranchesPage::requestSwitch(const QString &id, const QString &ref, std::function<void()> continuation)
{
    if (m_busy || !m_service->contains(id)) return;
    m_busy = true; updateActions();
    m_task = m_service->branches(id, this, [this, id, ref, continuation](const BackupResult<BranchSnapshot> &snapshot)
    {
        if (!snapshot.result.success) { complete(snapshot.result); return; }
        BranchRequest request; request.context = snapshot.value.context; request.ref = ref;
        for (const auto &branch : snapshot.value.branches) if (branch.ref == ref) request.expectedHead = branch.head;
        m_task = m_service->prepareBranchSwitch(request, this, [this, id, continuation](const BackupResult<PreparedBranchSwitch> &reply)
        {
            if (!reply.result.success || reply.value.targetRef.isEmpty()) { complete(reply.result); return; }
            const auto prepared = reply.value;
            const auto from = prepared.context.ref.mid(11), to = prepared.targetRef.mid(11);
            const QPointer<HomePageBranchesPage> guard(this);
            UiDialog::Dialog dialog(dialogOwner(), "switchBranchDialog", "切换方案");
            auto *label = new QLabel(QString("从“%1”切换到“%2”。\n文件夹 %3 里的文件会变成“%2”的内容。%4")
                .arg(from, to, m_service->sourcePath(id),
                     prepared.savesChanges ? QString("\n\n你还没保存的修改会先保存到“%1”，不会丢失。").arg(from) : QString()), &dialog);
            label->setWordWrap(true); dialog.contentLayout()->addWidget(label);
            auto *details = new QPushButton(QString("查看将变化的文件（%1 个）").arg(prepared.changes.size() + prepared.savedChanges.size()), &dialog);
            details->setObjectName("branchChangesToggle"); details->setCheckable(true); details->setFlat(true);
            dialog.contentLayout()->addWidget(details, 0, Qt::AlignLeft);
            auto *list = new QPlainTextEdit((prepared.savesChanges ? "先保存到“" + from + "”：\n" + changeList(prepared.savedChanges) + "\n\n" : QString()) + "切换后文件变化：\n" + changeList(prepared.changes), &dialog);
            list->setReadOnly(true); list->setObjectName("branchChanges"); list->setVisible(false); dialog.contentLayout()->addWidget(list);
            connect(details, &QPushButton::toggled, list, &QWidget::setVisible);
            auto *buttons = dialogButtons(dialog);
            buttons->button(QDialogButtonBox::Ok)->setText(prepared.savesChanges ? "保存并切换" : "切换方案");
            const auto accepted = dialog.exec() == QDialog::Accepted;
            if (!guard) return;
            if (!accepted) { m_busy = false; m_task = 0; updateActions(); return; }
            m_task = m_service->switchBranch(prepared, this, [this, continuation](const OperationResult &result) { complete(result); if (result.success && continuation) continuation(); });
        });
    });
}
void HomePageBranchesPage::showOperations()
{
    const auto selected = selectedBranch();
    if (selected.ref.isEmpty()) return;
    const auto selectedRequest = request(selected);
    const auto branches = m_snapshot.branches;
    const QPointer<HomePageBranchesPage> guard(this);
    QMenu menu(this);
    QAction *renameAction = nullptr, *removeAction = nullptr, *mergeAction = nullptr;
    if (!selected.remoteBranch)
    {
        renameAction = menu.addAction("重命名…");
        removeAction = menu.addAction("删除…"); removeAction->setEnabled(!selected.current);
        if (selected.current) removeAction->setToolTip("正在使用的方案不能删除，请先切换到其他方案。");
        menu.addSeparator();
    }
    auto *advanced = menu.addMenu("高级");
    auto *compareAction = advanced->addAction("与其他方案比较…");
    if (!selected.remoteBranch)
    {
        mergeAction = advanced->addAction("把这个方案的修改合并到正在使用的方案…"); mergeAction->setEnabled(!selected.current);
    }
    const auto chosen = menu.exec(ui->listOperations->mapToGlobal(QPoint(0, ui->listOperations->height())));
    if (!guard || !chosen) return;
    if (chosen == compareAction) compare(selected, selectedRequest, branches);
    else if (chosen == mergeAction) merge(selectedRequest);
    else if (chosen == renameAction) rename(selected);
    else if (chosen == removeAction && UiDialog::confirm(dialogOwner(), QString("删除方案“%1”？\n云端的同名方案不受影响。").arg(selected.name), "删除方案") && guard)
        remove(selected, false);
}
void HomePageBranchesPage::rename(const BranchInfo &selected)
{
    const QPointer<HomePageBranchesPage> guard(this);
    bool ok = false;
    const auto name = UiDialog::getText(dialogOwner(), "重命名方案", "新名称（只修改本机名称，云端名称保持不变）", selected.name, &ok);
    if (!guard || !ok) return;
    auto renamed = request(selected); renamed.name = name; m_busy = true; updateActions();
    m_task = m_service->renameBranch(renamed, this, [this](const OperationResult &result) { complete(result); });
}
void HomePageBranchesPage::remove(const BranchInfo &selected, bool force)
{
    auto removed = request(selected); removed.force = force; m_busy = true; updateActions();
    m_task = m_service->deleteBranch(removed, this, [this, selected, force](const OperationResult &result) {
        if (!result.success && !force && result.title == QString::fromUtf8(BranchNotMergedTitle))
        {
            m_busy = false; m_task = 0; updateActions();
            const QPointer<HomePageBranchesPage> guard(this);
            if (UiDialog::confirm(dialogOwner(), QString("方案“%1”里有些版本只存在于这个方案中，删除后可能无法找回。\n仍然删除？").arg(selected.name), "仍然删除") && guard)
                remove(selected, true);
            return;
        }
        complete(result);
    });
}
void HomePageBranchesPage::compare(const BranchInfo &selected, const BranchRequest &request, const QVector<BranchInfo> &branches)
{
    const QPointer<HomePageBranchesPage> guard(this);
    UiDialog::Dialog dialog(dialogOwner(), "compareBranchesDialog", "比较方案");
    auto *label = new QLabel("查看“" + displayName(selected) + "”和另一个方案的差别。选择另一个方案：", &dialog); label->setWordWrap(true); dialog.contentLayout()->addWidget(label);
    auto *combo = new QComboBox(&dialog);
    for (const auto &branch : branches)
        if (branch.ref != selected.ref && !isLinkedRemote(branch, branches)) combo->addItem(displayName(branch) + (branch.remoteBranch ? "（云端）" : QString()), branch.head);
    dialog.contentLayout()->addWidget(combo); dialogButtons(dialog);
    if (!combo->count() || dialog.exec() != QDialog::Accepted || !guard) return;
    Route route{PageId::Diff, request.context.id, combo->currentData().toString()}; route.oldCommit = selected.head; emit navigate(route);
}
void HomePageBranchesPage::merge(const BranchRequest &request)
{
    const QPointer<HomePageBranchesPage> guard(this);
    if (!UiDialog::confirm(dialogOwner(), QString("把“%1”的修改合并到正在使用的“%2”？\n接下来会先检查两边的差别，你确认预览后才会改动文件。“%1”本身保持不变。").arg(request.ref.mid(11), request.context.ref.mid(11)), "检查差别") || !guard) return;
    m_busy = true; updateActions();
    m_task = m_service->prepareBranchMerge(request, this, [this, id = request.context.id, source = request.context.ref](const BackupResult<SyncResolutionSession> &result)
    {
        complete(result.result);
        if (result.result.success && !result.value.id.isEmpty())
        {
            Route route{PageId::Conflict, id}; route.branchRef = source; emit navigate(route);
        }
    });
}
