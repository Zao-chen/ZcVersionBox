#include "homepage_page_branches.h"
#include "homepage_page_branches_row.h"
#include "ui_homepage_page_branches.h"
#include "windows/mainwindow_dialog.h"
#include "windows/mainwindow_presentation.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace
{
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
QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName("branchSection");
    label->setContentsMargins(16, 10, 0, 2);
    UiStyle::text(label, UiStyle::FontRole::Caption, true);
    return label;
}
}

HomePageBranchesPage::HomePageBranchesPage(BackupService *service, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePageBranchesPage), m_service(service)
{
    ui->setupUi(this);
    ui->branchScroll->viewport()->setAutoFillBackground(false);
    ui->branchList->setAutoFillBackground(false);
    ui->listCreate->setIcon(UiStyle::icon("add"));
    m_refresh = UiStyle::action(this, "refreshBranchesAction", "刷新", "refresh");
    m_refresh->setProperty("iconOnly", true);
    connect(m_refresh, &QAction::triggered, this, &HomePageBranchesPage::refresh);
    connect(ui->listCreate, &QPushButton::clicked, this, [this] { createFrom(m_id); });
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
    const auto narrow = width() < 640;
    ui->layout->setContentsMargins(narrow ? 12 : 20, 8, narrow ? 16 : 24, 16);
    if (m_id != id)
    {
        ++m_generation;
        m_snapshot = {};
        clearList();
    }
    m_id = id;
    refresh();
}
void HomePageBranchesPage::refresh()
{
    if (m_id.isEmpty() || m_busy) return;
    const auto generation = ++m_generation;
    const auto id = m_id;
    setLoading(true);
    m_service->branches(id, this, [this, id, generation](const BackupResult<BranchSnapshot> &reply)
    {
        if (id != m_id || generation != m_generation) return;
        setLoading(false);
        if (!reply.result.success) { if (!reply.result.cancelled) emit notification(reply.result); return; }
        m_snapshot = reply.value;
        populateList();
        updateActions();
    });
}
void HomePageBranchesPage::setLoading(bool loading)
{
    if (m_loading != loading)
    {
        m_loading = loading;
        emit loadingChanged(loading);
    }
    if (loading && m_rows.isEmpty()) clearList("正在读取方案…");
    else if (!loading && m_snapshot.branches.isEmpty()) clearList();
}
void HomePageBranchesPage::clearList(const QString &placeholder)
{
    m_rows.clear();
    while (auto *item = ui->branchRows->takeAt(0))
    {
        if (auto *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    if (placeholder.isEmpty()) return;
    ui->branchRows->addWidget(sectionLabel(placeholder, ui->branchList));
    ui->branchRows->addStretch(1);
}
void HomePageBranchesPage::populateList()
{
    const auto *focused = qobject_cast<HomePageBranchRow *>(focusWidget());
    const auto focusedRef = focused ? focused->branch().ref : QString();
    clearList();
    QVector<BranchInfo> current, other;
    for (const auto &branch : m_snapshot.branches)
    {
        if (isLinkedRemote(branch, m_snapshot.branches)) continue;
        (branch.current ? current : other).append(branch);
    }
    HomePageBranchRow *restore = nullptr;
    const auto addSection = [&](const QString &title, const QVector<BranchInfo> &branches)
    {
        if (branches.isEmpty()) return;
        ui->branchRows->addWidget(sectionLabel(title, ui->branchList));
        for (const auto &branch : branches)
        {
            auto *row = new HomePageBranchRow(branch, displayName(branch), ui->branchList);
            connect(row, &HomePageBranchRow::useRequested, this, [this, branch] { useBranch(branch); });
            connect(row, &HomePageBranchRow::historyRequested, this, [this, branch] { showHistory(branch); });
            connect(row, &HomePageBranchRow::menuRequested, this, [this, branch](const QPoint &pos) { showOperations(branch, pos); });
            connect(row, &HomePageBranchRow::stepRequested, this, [this, row](int delta)
            {
                const auto next = m_rows.indexOf(row) + delta;
                if (next >= 0 && next < m_rows.size())
                {
                    m_rows[next]->setFocus(Qt::TabFocusReason);
                    ui->branchScroll->ensureWidgetVisible(m_rows[next]);
                }
            });
            if (branch.ref == focusedRef) restore = row;
            ui->branchRows->addWidget(row);
            m_rows.append(row);
        }
    };
    addSection("正在使用", current);
    addSection("其他方案", other);
    ui->branchRows->addStretch(1);
    if (restore) restore->setFocus(Qt::OtherFocusReason);
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
    const bool tracking = !m_id.isEmpty() && m_service->contains(m_id) && m_service->syncState(m_id) == BackupSyncState::Tracking;
    const auto reason = tracking ? QString() : QString("当前状态：%1。处理完成后才能管理方案。").arg(m_id.isEmpty() ? QString() : backupStateText(m_service->syncState(m_id)));
    m_refresh->setEnabled(!m_id.isEmpty() && !m_busy);
    ui->listCreate->setEnabled(!m_busy && tracking);
    ui->listCreate->setToolTip(reason);
    for (auto *row : std::as_const(m_rows)) row->setActionsEnabled(!m_busy && tracking, reason);
    ui->cancelTask->setVisible(m_busy);
}
void HomePageBranchesPage::useBranch(const BranchInfo &branch)
{
    if (branch.ref.isEmpty() || branch.current) return;
    requestSwitch(m_id, branch.ref);
}
// Viewing one 方案 reuses the history page's scope filter instead of a separate page.
void HomePageBranchesPage::showHistory(const BranchInfo &branch)
{
    Route route{PageId::History, m_id}; route.branchRef = branch.ref; emit navigate(route);
}
void HomePageBranchesPage::createFrom(const QString &id, const QString &commit)
{
    create(id, commit);
}
void HomePageBranchesPage::ensureLocal(const QString &id, const BranchInfo &branch, std::function<void(const BranchInfo &)> continuation)
{
    if (!branch.remoteBranch) { continuation(branch); return; }
    m_busy = true; updateActions();
    BranchRequest adopted;
    adopted.context = m_service->branchContext(id); adopted.name = displayName(branch);
    adopted.startCommit = branch.head; adopted.upstream = branch.ref;
    m_task = m_service->createBranch(adopted, this, [this, id, name = adopted.name, continuation](const OperationResult &result) {
        if (!result.success) { complete(result); return; }
        m_task = m_service->branches(id, this, [this, id, name, continuation](const BackupResult<BranchSnapshot> &snapshot) {
            m_busy = false; m_task = 0; updateActions();
            if (!snapshot.result.success) { complete(snapshot.result); return; }
            if (id == m_id) m_snapshot = snapshot.value;
            else refresh();
            for (const auto &local : snapshot.value.branches)
                if (!local.remoteBranch && local.name == name) { continuation(local); return; }
            complete(OperationResult::warn("方案已变化", "请刷新方案后重试。"));
        });
    });
}
void HomePageBranchesPage::create(const QString &id, const QString &commit)
{
    if (m_busy || !m_service->contains(id)) return;
    const auto context = m_service->branchContext(id);
    const QPointer<HomePageBranchesPage> guard(this);
    UiDialog::Dialog dialog(dialogOwner(), "createBranchDialog", "新建方案");
    const auto text = commit.isEmpty() || commit == context.head ? QStringLiteral("从当前方案的最新版本开始一个新方案。原方案不受影响，随时可以切换回来。")
                    : QStringLiteral("从所选的版本开始一个新方案。原方案不受影响，随时可以切换回来。");
    auto *description = new QLabel(text, &dialog);
    description->setWordWrap(true); dialog.contentLayout()->addWidget(description);
    auto *name = new QLineEdit(&dialog); name->setObjectName("branchName"); name->setPlaceholderText("例如：尝试新排版");
    dialog.contentLayout()->addWidget(name);
    auto *rule = new QLabel("可以使用中文；不能包含空格以及 ~ ^ : ? * [ \\ 等符号。", &dialog);
    rule->setWordWrap(true); UiStyle::text(rule, UiStyle::FontRole::Caption, true); dialog.contentLayout()->addWidget(rule);
    auto *use = new QCheckBox("创建后立即使用（会先保存当前修改）", &dialog); use->setChecked(true); dialog.contentLayout()->addWidget(use);
    auto *buttons = dialogButtons(dialog); buttons->button(QDialogButtonBox::Ok)->setText("创建方案");
    if (dialog.exec() != QDialog::Accepted || !guard) return;
    BranchRequest request; request.context = context; request.name = name->text(); request.startCommit = commit.isEmpty() ? context.head : commit;
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
        for (const auto &branch : snapshot.value.branches)
            if (branch.ref == ref && branch.remoteBranch)
            {
                m_busy = false;
                ensureLocal(id, branch, [this, id, continuation](const BranchInfo &local) { requestSwitch(id, local.ref, continuation); });
                return;
            }
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
void HomePageBranchesPage::showOperations(const BranchInfo &selected, const QPoint &globalPos)
{
    if (selected.ref.isEmpty()) return;
    const auto selectedRequest = request(selected);
    const auto branches = m_snapshot.branches;
    const QPointer<HomePageBranchesPage> guard(this);
    const bool editable = !m_busy && m_service->contains(m_id) && m_service->syncState(m_id) == BackupSyncState::Tracking;
    QMenu menu(this);
    QAction *useAction = nullptr, *mergeAction = nullptr, *renameAction = nullptr, *removeAction = nullptr;
    auto *historyAction = menu.addAction("查看历史");
    if (!selected.current) useAction = menu.addAction("切换到此方案…");
    menu.addSeparator();
    auto *createAction = menu.addAction("从此方案新建…");
    auto *compareAction = menu.addAction("与其他方案比较…");
    if (!selected.current) mergeAction = menu.addAction("合并到正在使用的方案…");
    {
        menu.addSeparator();
        renameAction = menu.addAction("重命名…");
        removeAction = menu.addAction("删除…"); removeAction->setEnabled(!selected.current);
        if (selected.current) removeAction->setToolTip("正在使用的方案不能删除，请先切换到其他方案。");
    }
    for (auto *action : {useAction, createAction, mergeAction, renameAction, removeAction})
        if (action && !editable) action->setEnabled(false);
    const auto chosen = menu.exec(globalPos);
    if (!guard || !chosen) return;
    if (chosen == historyAction) showHistory(selected);
    else if (chosen == useAction) useBranch(selected);
    else if (chosen == createAction) createFrom(m_id, selected.head);
    else if (chosen == compareAction) compare(selected, selectedRequest, branches);
    else if (chosen == mergeAction) ensureLocal(m_id, selected, [this, id = m_id](const BranchInfo &local) { if (id == m_id) merge(request(local)); });
    else if (chosen == renameAction) ensureLocal(m_id, selected, [this, id = m_id](const BranchInfo &local) { if (id == m_id) rename(local); });
    else if (chosen == removeAction && UiDialog::confirm(dialogOwner(), QString("删除方案“%1”？\n此删除会同步到共享仓库及所有协作者。其他方案不受影响。").arg(displayName(selected)), "删除共享方案") && guard)
        ensureLocal(m_id, selected, [this, id = m_id](const BranchInfo &local) { if (id == m_id) remove(local, false); });
}
void HomePageBranchesPage::rename(const BranchInfo &selected)
{
    const QPointer<HomePageBranchesPage> guard(this);
    bool ok = false;
    const auto name = UiDialog::getText(dialogOwner(), "重命名方案", "新名称（同步到所有协作者）", selected.name, &ok);
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
            if (UiDialog::confirm(dialogOwner(), QString("方案“%1”包含尚未合入其他方案的版本。删除会同步到所有协作者，建议先合并需要保留的内容。\n仍然删除？").arg(selected.name), "仍然删除") && guard)
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
        if (branch.ref != selected.ref && !isLinkedRemote(branch, branches)) combo->addItem(displayName(branch), branch.head);
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

void HomePageBranchesPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const auto narrow = width() < 640;
    ui->layout->setContentsMargins(narrow ? 12 : 20, 8, narrow ? 16 : 24, 16);
}
