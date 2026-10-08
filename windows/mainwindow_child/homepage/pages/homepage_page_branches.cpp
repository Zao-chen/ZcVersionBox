#include "homepage_page_branches.h"
#include "homepage_page_backup.h"
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
#include <QSignalBlocker>
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
}

HomePageBranchesPage::HomePageBranchesPage(BackupService *service, QWidget *parent)
    : QWidget(parent), ui(new Ui::HomePageBranchesPage), m_service(service)
{
    ui->setupUi(this);
    UiStyle::text(ui->branchName, UiStyle::FontRole::Object);
    UiStyle::text(ui->branchState, UiStyle::FontRole::Caption, true);
    UiStyle::text(ui->branchDetails, UiStyle::FontRole::Caption, true);
    m_history = new HomePageBackupPage(service, ui->historyContainer);
    ui->historyLayout->addWidget(m_history);
    m_create = UiStyle::action(this, "createBranchAction", "新建方案…", "add");
    m_refresh = UiStyle::action(this, "refreshBranchesAction", "刷新", "refresh");
    m_fetch = UiStyle::action(this, "fetchBranchesAction", "获取云端方案", "cloud");
    connect(m_create, &QAction::triggered, this, [this] { createFrom(m_id); });
    connect(m_refresh, &QAction::triggered, this, &HomePageBranchesPage::refresh);
    connect(m_fetch, &QAction::triggered, this, [this]
    {
        if (m_busy || m_id.isEmpty()) return;
        m_busy = true; updateActions();
        m_task = m_service->fetchBranches(m_service->branchContext(m_id), this, [this](const OperationResult &r) { complete(r); });
    });
    connect(ui->useBranch, &QPushButton::clicked, this, [this]
    {
        if (m_branch.remoteBranch) createFrom(m_id, m_branch.head);
        else requestSwitch(m_id, m_branch.ref);
    });
    connect(ui->operations, &QPushButton::clicked, this, &HomePageBranchesPage::showOperations);
    connect(ui->cancelTask, &QPushButton::clicked, this, [this] { if (m_task) m_service->cancel(m_task); });
    connect(m_history, &HomePageBackupPage::navigate, this, &HomePageBranchesPage::navigate);
    connect(m_history, &HomePageBackupPage::notification, this, &HomePageBranchesPage::notification);
    connect(m_history, &HomePageBackupPage::createBranchRequested, this, &HomePageBranchesPage::createFrom);
    connect(m_history, &HomePageBackupPage::branchRequested, this, [this](const QString &ref)
    {
        if (!ref.isEmpty() && ref != m_branchRef) { Route route{PageId::Branches, m_id}; route.branchRef = ref; emit navigate(route); }
    });
    connect(m_history, &HomePageBackupPage::commonAncestorsExpanded, this, [this] { m_historyExpanded = true; });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id)
    {
        if (id == m_id && isVisible() && !m_busy) refresh();
    });
    updateActions();
}
HomePageBranchesPage::~HomePageBranchesPage() = default;
QList<QAction *> HomePageBranchesPage::toolbarActions() const { return {m_create, m_fetch, m_refresh}; }
QWidget *HomePageBranchesPage::dialogOwner() const { return const_cast<HomePageBranchesPage *>(this); }
BranchRequest HomePageBranchesPage::request() const
{
    BranchRequest result;
    result.context = m_snapshot.context;
    result.ref = m_branch.ref;
    result.expectedHead = m_branch.head;
    result.endpoint = m_branch.endpoint;
    return result;
}
void HomePageBranchesPage::setBackup(const QString &id, const QString &branchRef)
{
    if (m_id != id || m_branchRef != branchRef)
    {
        ++m_generation;
        m_snapshot = {};
        m_branch = {};
    }
    m_id = id;
    m_branchRef = branchRef;
    m_historyExpanded = false;
    if (m_history) m_history->setBranchDetail(id, branchRef, false);
    refresh();
}
void HomePageBranchesPage::refresh()
{
    if (m_id.isEmpty() || m_busy) return;
    const auto generation = ++m_generation;
    const auto id = m_id;
    const auto ref = m_branchRef;
    m_service->branches(id, this, [this, id, ref, generation](const BackupResult<BranchSnapshot> &reply)
    {
        if (id != m_id || ref != m_branchRef || generation != m_generation) return;
        if (!reply.result.success) { ui->status->setText(reply.result.message); return; }
        m_snapshot = reply.value;
        m_branch = {};
        for (const auto &branch : reply.value.branches)
            if (branch.ref == ref) { m_branch = branch; break; }
        if (m_branch.ref.isEmpty())
        {
            emit notification(OperationResult::warn("方案不存在", "该方案可能已被删除或远程列表已变化。"));
            emit replaceRoute({PageId::History, id});
            return;
        }
        ui->branchName->setText(m_branch.name);
        ui->branchState->setText(m_branch.current ? "正在使用" : m_branch.remoteBranch ? "云端方案" : "本地方案");
        const auto upstream = m_branch.upstream.isEmpty() ? "未关联云端" : "对应云端：" + m_branch.upstream.mid(13);
        ui->branchDetails->setText(QString("当前提交：%1\n%2\n引用：%3").arg(m_branch.head.left(8), upstream, m_branch.ref));
        ui->status->setText(m_branch.message.isEmpty() ? "方案历史" : m_branch.message);
        if (m_history) m_history->setBranchDetail(id, ref, m_historyExpanded);
        updateActions();
    });
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
    const bool available = !m_id.isEmpty() && !m_branch.ref.isEmpty() && m_service->contains(m_id);
    const bool editable = available && !m_busy && m_service->syncState(m_id) == BackupSyncState::Tracking;
    m_create->setEnabled(editable);
    m_fetch->setEnabled(editable);
    m_refresh->setEnabled(available && !m_busy);
    ui->useBranch->setEnabled(editable && !m_branch.current);
    ui->useBranch->setText(m_branch.remoteBranch ? "加入并使用…" : "使用此方案…");
    ui->operations->setEnabled(editable);
    ui->cancelTask->setVisible(m_busy);
}
void HomePageBranchesPage::createFrom(const QString &id, const QString &commit)
{
    if (m_busy || !m_service->contains(id)) return;
    const auto context = m_service->branchContext(id);
    const QPointer<HomePageBranchesPage> guard(this);
    const bool tracking = id == m_id && m_branch.remoteBranch && m_branch.head == commit;
    const auto upstream = tracking ? m_branch.ref : QString();
    UiDialog::Dialog dialog(dialogOwner(), "createBranchDialog", "新建方案");
    auto *description = new QLabel("从已保存版本 " + (commit.isEmpty() ? context.head : commit).left(8) + " 创建方案。名称与 Git 分支名一致，可使用中文。", &dialog);
    description->setWordWrap(true); dialog.contentLayout()->addWidget(description);
    auto *name = new QLineEdit(&dialog); name->setObjectName("branchName"); name->setPlaceholderText("例如：尝试新排版");
    if (tracking) name->setText(m_branch.remoteRef.mid(11));
    dialog.contentLayout()->addWidget(name);
    auto *use = new QCheckBox("创建后切换（切换前会保存当前修改）", &dialog); use->setChecked(true); dialog.contentLayout()->addWidget(use);
    auto *buttons = dialogButtons(dialog); buttons->button(QDialogButtonBox::Ok)->setText("创建方案");
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
void HomePageBranchesPage::requestSwitch(const QString &id, const QString &ref)
{
    if (m_busy || !m_service->contains(id)) return;
    m_busy = true; updateActions();
    m_task = m_service->branches(id, this, [this, id, ref](const BackupResult<BranchSnapshot> &snapshot)
    {
        if (!snapshot.result.success) { complete(snapshot.result); return; }
        BranchRequest request; request.context = snapshot.value.context; request.ref = ref;
        for (const auto &branch : snapshot.value.branches) if (branch.ref == ref) request.expectedHead = branch.head;
        m_task = m_service->prepareBranchSwitch(request, this, [this, id](const BackupResult<PreparedBranchSwitch> &reply)
        {
            if (!reply.result.success || reply.value.targetRef.isEmpty()) { complete(reply.result); return; }
            const auto prepared = reply.value;
            const QPointer<HomePageBranchesPage> guard(this);
            UiDialog::Dialog dialog(dialogOwner(), "switchBranchDialog", "切换方案");
            auto *label = new QLabel(QString("%1 → %2\n源位置：%3\n\n%4")
                .arg(prepared.context.ref.mid(11), prepared.targetRef.mid(11), m_service->sourcePath(id),
                     prepared.savesChanges ? "当前修改将先保存到原方案，再切换原位置的文件。" : "切换会更新原位置的文件。没有新修改，不会新增版本。"), &dialog);
            label->setWordWrap(true); dialog.contentLayout()->addWidget(label);
            auto *list = new QPlainTextEdit((prepared.savesChanges ? "将保存到原方案：\n" + changeList(prepared.savedChanges) + "\n\n" : QString()) + "切换后源文件变化：\n" + changeList(prepared.changes), &dialog);
            list->setReadOnly(true); list->setObjectName("branchChanges"); dialog.contentLayout()->addWidget(list);
            auto *buttons = dialogButtons(dialog);
            buttons->button(QDialogButtonBox::Ok)->setText(prepared.savesChanges ? "保存并切换" : "切换方案");
            const auto accepted = dialog.exec() == QDialog::Accepted;
            if (!guard) return;
            if (!accepted) { m_busy = false; m_task = 0; updateActions(); return; }
            m_task = m_service->switchBranch(prepared, this, [this](const OperationResult &result) { complete(result); });
        });
    });
}
void HomePageBranchesPage::showOperations()
{
    if (m_branch.ref.isEmpty()) return;
    const auto selected = m_branch;
    const auto selectedRequest = request();
    const auto branches = m_snapshot.branches;
    const QPointer<HomePageBranchesPage> guard(this);
    QMenu menu(this);
    auto *compareAction = menu.addAction("与其他方案比较…");
    QAction *mergeAction = nullptr, *renameAction = nullptr, *upstreamAction = nullptr, *pushAction = nullptr, *publishAction = nullptr;
    if (!selected.remoteBranch)
    {
        mergeAction = menu.addAction("合并到正在使用的方案…"); mergeAction->setEnabled(!selected.current);
        renameAction = menu.addAction("重命名…");
        upstreamAction = menu.addAction("设置对应云端…");
        pushAction = menu.addAction("上传此方案的已保存版本");
        publishAction = menu.addAction("发布为同名云端方案…");
    }
    menu.addSeparator();
    auto *removeAction = menu.addAction(selected.remoteBranch ? "删除云端方案…" : "删除本地方案…"); removeAction->setEnabled(!selected.current);
    auto *forceAction = selected.remoteBranch ? nullptr : menu.addAction("仍然删除未合并方案…");
    if (forceAction) forceAction->setEnabled(!selected.current);
    const auto chosen = menu.exec(ui->operations->mapToGlobal(QPoint(0, ui->operations->height())));
    if (!guard || !chosen) return;
    if (chosen == compareAction) { compare(selected, selectedRequest, branches); return; }
    if (chosen == mergeAction) { merge(selectedRequest); return; }
    if (chosen == renameAction)
    {
        bool ok = false;
        const auto name = UiDialog::getText(dialogOwner(), "重命名方案", "新名称（云端名称不会自动修改）", selected.name, &ok);
        if (!guard || !ok) return;
        auto renamed = selectedRequest; renamed.name = name; m_busy = true; updateActions();
        m_task = m_service->renameBranch(renamed, this, [this, name, id = selectedRequest.context.id, ref = selectedRequest.ref](const OperationResult &result)
        {
            complete(result);
            if (result.success && m_id == id && m_branchRef == ref)
            {
                Route route{PageId::Branches, id}; route.branchRef = "refs/heads/" + name;
                emit replaceRoute(route);
            }
        });
        return;
    }
    if (chosen == upstreamAction)
    {
        UiDialog::Dialog dialog(dialogOwner(), "branchUpstreamDialog", "设置对应云端");
        auto *combo = new QComboBox(&dialog); combo->addItem("不关联云端", QString());
        for (const auto &branch : branches) if (branch.remoteBranch) combo->addItem(branch.name, branch.ref);
        combo->setCurrentIndex(qMax(0, combo->findData(selected.upstream)));
        dialog.contentLayout()->addWidget(combo); dialogButtons(dialog);
        if (dialog.exec() != QDialog::Accepted || !guard) return;
        auto linked = selectedRequest; linked.upstream = combo->currentData().toString(); m_busy = true; updateActions();
        m_task = m_service->setBranchUpstream(linked, this, [this](const OperationResult &result) { complete(result); });
        return;
    }
    if (chosen == pushAction || chosen == publishAction)
    {
        auto upload = selectedRequest;
        if (chosen == publishAction)
        {
            if (!UiDialog::confirm(dialogOwner(), "将已保存版本发布为云端方案“" + selected.name + "”？旧的云端方案会保留。", "发布方案") || !guard) return;
            upload.name = selected.name;
        }
        m_busy = true; updateActions();
        m_task = m_service->uploadBranch(upload, this, [this](const OperationResult &result) { complete(result); });
        return;
    }
    const bool force = chosen == forceAction;
    const auto text = selected.remoteBranch ? QString("删除云端方案“%1”？本地方案仍会保留。").arg(selected.name)
        : force ? QString("仍然删除“%1”？尚未合并的独有版本可能不再可恢复。").arg(selected.name)
                : QString("删除本地方案“%1”？Git 会检查是否已合并；尚未合并时会停止。").arg(selected.name);
    if (!UiDialog::confirm(dialogOwner(), text, "删除方案") || !guard) return;
    auto removed = selectedRequest; removed.force = force; m_busy = true; updateActions();
    const auto deleted = [this, id = removed.context.id, ref = removed.ref](const OperationResult &result) {
        const bool leaving = result.success && m_id == id && m_branchRef == ref;
        if (leaving)
        {
            ++m_generation;
            m_history->deactivate();
        }
        complete(result, !leaving);
        if (leaving) emit replaceRoute({PageId::History, id});
    };
    m_task = selected.remoteBranch ? m_service->deleteRemoteBranch(removed, this, deleted)
                                  : m_service->deleteBranch(removed, this, deleted);
}
void HomePageBranchesPage::compare(const BranchInfo &selected, const BranchRequest &request, const QVector<BranchInfo> &branches)
{
    const QPointer<HomePageBranchesPage> guard(this);
    UiDialog::Dialog dialog(dialogOwner(), "compareBranchesDialog", "比较方案");
    auto *label = new QLabel("修改前：" + selected.name + "\n选择修改后版本：", &dialog); label->setWordWrap(true); dialog.contentLayout()->addWidget(label);
    auto *combo = new QComboBox(&dialog);
    for (const auto &branch : branches) if (branch.ref != selected.ref) combo->addItem(branch.name, branch.head);
    dialog.contentLayout()->addWidget(combo); dialogButtons(dialog);
    if (!combo->count() || dialog.exec() != QDialog::Accepted || !guard) return;
    Route route{PageId::Diff, request.context.id, combo->currentData().toString()}; route.oldCommit = selected.head; emit navigate(route);
}
void HomePageBranchesPage::merge(const BranchRequest &request)
{
    const QPointer<HomePageBranchesPage> guard(this);
    if (!UiDialog::confirm(dialogOwner(), QString("将“%1”合并到正在使用的“%2”？\n接下来会分析差异，预览确认后才应用到源文件。来源方案会保留。").arg(request.ref.mid(11), request.context.ref.mid(11)), "分析合并") || !guard) return;
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
