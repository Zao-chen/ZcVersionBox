#include "homepage_page_branches.h"
#include "ui_homepage_page_branches.h"
#include "windows/mainwindow_dialog.h"
#include "windows/mainwindow_presentation.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace
{
QDialogButtonBox *dialogButtons(UiDialog::Dialog &dialog)
{
    auto *buttons = dialog.buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    dialog.contentLayout()->addWidget(buttons);
    dialog.bindButtonBox(buttons);
    return buttons;
}
constexpr int RefRole = Qt::UserRole + 1;
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
    : QWidget(parent), ui(new Ui::HomePageBranchesPage), m_service(service), m_owner(parent)
{
    ui->setupUi(this);
    m_create = UiStyle::action(this, "createBranchAction", "新建方案…", "add");
    m_refresh = UiStyle::action(this, "refreshBranchesAction", "刷新", "refresh");
    m_fetch = UiStyle::action(this, "fetchBranchesAction", "获取云端方案", "cloud");
    m_manage = UiStyle::action(this, "manageBranchesAction", "管理方案…", "more");
    ui->createButton->setDefaultAction(m_create); ui->fetchButton->setDefaultAction(m_fetch); ui->refreshButton->setDefaultAction(m_refresh);
    ui->branches->setModel(&m_branches);
    UiStyle::flatView(ui->branches); ui->branches->verticalHeader()->hide();
    ui->branches->verticalHeader()->setDefaultSectionSize(40);
    ui->branches->horizontalHeader()->setStretchLastSection(true);
    ui->branches->setAccessibleName("方案列表");
    connect(m_manage, &QAction::triggered, this, &HomePageBranchesPage::openManager);
    connect(this, &HomePageBranchesPage::navigate, this, [this] { if (m_dialog) m_dialog->hide(); });
    connect(m_create, &QAction::triggered, this, [this] { createFrom(m_id); });
    connect(m_refresh, &QAction::triggered, this, &HomePageBranchesPage::refresh);
    connect(m_fetch, &QAction::triggered, this, [this]
    {
        m_busy = true; updateActions();
        m_task = m_service->fetchBranches(m_service->branchContext(m_id), this, [this](const OperationResult &r) { complete(r); });
    });
    connect(ui->branches->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateActions(); });
    connect(ui->viewHistory, &QPushButton::clicked, this, [this]
    { const auto b = selected(); if (!b.ref.isEmpty()) { Route r{PageId::History, m_id}; r.branchRef = b.ref; emit navigate(r); } });
    connect(ui->branches, &QTableView::doubleClicked, ui->viewHistory, &QPushButton::click);
    connect(ui->useBranch, &QPushButton::clicked, this, [this]
    {
        const auto b = selected();
        if (b.remoteBranch) createFrom(m_id, b.head);
        else requestSwitch(m_id, b.ref);
    });
    connect(ui->operations, &QPushButton::clicked, this, &HomePageBranchesPage::showOperations);
    connect(ui->cancelTask, &QPushButton::clicked, this, [this] { if (m_task) m_service->cancel(m_task); });
    connect(service, &BackupService::repositoryChanged, this, [this](const QString &id)
    { if (id == m_id && isVisible() && !m_busy) refresh(); });
    updateActions();
}
HomePageBranchesPage::~HomePageBranchesPage() = default;
QList<QAction *> HomePageBranchesPage::toolbarActions() const { return {m_create, m_fetch, m_manage}; }
QWidget *HomePageBranchesPage::dialogOwner() const
{
    return m_dialog && m_dialog->isVisible() ? m_dialog.data() : m_owner.data();
}
void HomePageBranchesPage::openManager()
{
    if (!m_service->contains(m_id)) return;
    if (!m_dialog)
    {
        auto *dialog = new UiDialog::Dialog(m_owner, "branchManagerDialog", "管理方案");
        m_dialog = dialog;
        dialog->resize(660, 460);
        auto *manager = new HomePageBranchesPage(m_service, dialog);
        m_dialogPage = manager;
        dialog->contentLayout()->addWidget(manager);
        connect(manager, &HomePageBranchesPage::navigate, this, [this](const Route &route)
        {
            if (m_dialog) m_dialog->hide();
            emit navigate(route);
        });
        connect(manager, &HomePageBranchesPage::notification, this, &HomePageBranchesPage::notification);
    }
    refresh();
    if (m_dialogPage) m_dialogPage->setBackup(m_id);
    m_dialog->show(); m_dialog->raise(); m_dialog->activateWindow();
}
void HomePageBranchesPage::setBackup(const QString &id)
{
    if (m_id != id) { ++m_generation; m_snapshot = {}; m_branches.clear(); }
    m_id = id; refresh();
}
BranchInfo HomePageBranchesPage::selected() const
{
    const auto ref = ui->branches->currentIndex().siblingAtColumn(0).data(RefRole).toString();
    for (const auto &b : m_snapshot.branches) if (b.ref == ref) return b;
    return {};
}
BranchRequest HomePageBranchesPage::request(const BranchInfo &b) const
{ BranchRequest r; r.context = m_snapshot.context; r.ref = b.ref; r.expectedHead = b.head; r.endpoint = b.endpoint; return r; }
void HomePageBranchesPage::refresh()
{
    if (m_id.isEmpty() || m_busy) return;
    const auto generation = ++m_generation; const auto id = m_id;
    m_service->branches(id, this, [this, id, generation](const BackupResult<BranchSnapshot> &r)
    {
        if (m_id != id || generation != m_generation) return;
        if (!r.result.success) { ui->status->setText(r.result.message); return; }
        const auto previous = selected().ref;
        m_snapshot = r.value; m_branches.clear();
        m_branches.setHorizontalHeaderLabels({"方案", "位置 / 状态", "最新版本"});
        int select = 0;
        for (const auto &b : r.value.branches)
        {
            auto *name = new QStandardItem(b.name); name->setData(b.ref, RefRole);
            name->setToolTip(b.ref + '\n' + b.head + (b.upstream.isEmpty() ? QString() : "\n对应云端：" + b.upstream.mid(13)));
            m_branches.appendRow({name, new QStandardItem(b.current ? "正在使用" : b.remoteBranch ? "云端" : "本地"), new QStandardItem(b.message)});
            if (b.ref == previous || (previous.isEmpty() && b.current)) select = m_branches.rowCount() - 1;
        }
        ui->branches->setColumnWidth(0, 160); ui->branches->setColumnWidth(1, 90);
        if (m_branches.rowCount()) ui->branches->setCurrentIndex(m_branches.index(select, 0));
        ui->status->setText(QString("%1 个方案 · %2").arg(m_branches.rowCount()).arg(r.value.fetchedAt.isValid()
            ? "云端更新于 " + r.value.fetchedAt.toLocalTime().toString("yyyy-MM-dd HH:mm") : "云端列表尚未获取"));
        updateActions();
    });
}
void HomePageBranchesPage::complete(const OperationResult &result)
{
    m_busy = false; m_task = 0; emit notification(result); updateActions(); refresh();
}
void HomePageBranchesPage::updateActions()
{
    const auto b = selected();
    const bool available = !m_id.isEmpty() && m_service->contains(m_id), editable = available && !m_busy && m_service->syncState(m_id) == BackupSyncState::Tracking;
    m_create->setEnabled(editable); m_fetch->setEnabled(editable); m_refresh->setEnabled(available && !m_busy);
    ui->useBranch->setEnabled(editable && !b.ref.isEmpty() && !b.current);
    ui->useBranch->setText(b.remoteBranch ? "加入并使用…" : "使用此方案…");
    ui->viewHistory->setEnabled(!b.ref.isEmpty()); ui->operations->setEnabled(editable && !b.ref.isEmpty());
    ui->details->setText(b.ref.isEmpty() ? QString() : b.name + (b.upstream.isEmpty() ? " · 尚未关联云端" : " → " + b.upstream.mid(13)));
    m_manage->setEnabled(available); ui->cancelTask->setVisible(m_busy);
}

void HomePageBranchesPage::createFrom(const QString &id, const QString &commit)
{
    if (m_busy || !m_service->contains(id)) return;
    const QPointer<HomePageBranchesPage> guard(this);
    const auto context = m_service->branchContext(id);
    const auto selectedBranch = selected();
    const bool tracking = id == m_id && selectedBranch.remoteBranch && selectedBranch.head == commit;
    UiDialog::Dialog dialog(dialogOwner(), "createBranchDialog", "新建方案");
    auto *description = new QLabel("从已保存版本 " + (commit.isEmpty() ? context.head : commit).left(8) + " 创建方案。名称与 Git 分支名一致，可使用中文。", &dialog);
    description->setTextFormat(Qt::PlainText); description->setWordWrap(true); dialog.contentLayout()->addWidget(description);
    auto *name = new QLineEdit(&dialog); name->setObjectName("branchName"); name->setPlaceholderText("例如：尝试新排版");
    if (tracking) name->setText(selectedBranch.remoteRef.mid(11));
    dialog.contentLayout()->addWidget(name);
    auto *use = new QCheckBox("创建后切换（切换前会保存当前修改）", &dialog); use->setChecked(true); dialog.contentLayout()->addWidget(use);
    auto *buttons = dialogButtons(dialog); buttons->button(QDialogButtonBox::Ok)->setText("创建方案");
    if (dialog.exec() != QDialog::Accepted || !guard) return;
    BranchRequest r; r.context = context; r.name = name->text(); r.startCommit = commit.isEmpty() ? context.head : commit;
    if (tracking) r.upstream = selectedBranch.ref;
    const bool switchAfter = use->isChecked();
    m_busy = true; updateActions();
    m_task = m_service->createBranch(r, this, [this, id, branch = r.name, switchAfter](const OperationResult &result)
    { complete(result); if (result.success && switchAfter) requestSwitch(id, "refs/heads/" + branch); });
}

void HomePageBranchesPage::requestSwitch(const QString &id, const QString &ref)
{
    if (m_busy || !m_service->contains(id)) return;
    m_busy = true; updateActions();
    m_task = m_service->branches(id, this, [this, id, ref](const BackupResult<BranchSnapshot> &snapshot)
    {
        if (!snapshot.result.success) { complete(snapshot.result); return; }
        BranchRequest r; r.context = snapshot.value.context; r.ref = ref;
        for (const auto &b : snapshot.value.branches) if (b.ref == ref) r.expectedHead = b.head;
        m_task = m_service->prepareBranchSwitch(r, this, [this, id](const BackupResult<PreparedBranchSwitch> &reply)
        {
            if (!reply.result.success || reply.value.targetRef.isEmpty()) { complete(reply.result); return; }
            const auto prepared = reply.value;
            const QPointer<HomePageBranchesPage> guard(this);
            UiDialog::Dialog dialog(dialogOwner(), "switchBranchDialog", "切换方案");
            dialog.resize(500, 390);
            auto *label = new QLabel(QString("%1 → %2\n源位置：%3\n\n%4")
                .arg(prepared.context.ref.mid(11), prepared.targetRef.mid(11), m_service->sourcePath(id),
                     prepared.savesChanges ? "当前修改将先保存到原方案，再切换原位置的文件。" : "切换会更新原位置的文件。没有新修改，不会新增版本。"), &dialog);
            label->setTextFormat(Qt::PlainText); label->setWordWrap(true); dialog.contentLayout()->addWidget(label);
            const auto summary = (prepared.savesChanges ? "将保存到原方案：\n" + changeList(prepared.savedChanges) + "\n\n" : QString())
                + "切换后源文件变化：\n" + changeList(prepared.changes);
            auto *list = new QPlainTextEdit(summary, &dialog); list->setReadOnly(true); list->setObjectName("branchChanges"); dialog.contentLayout()->addWidget(list);
            auto *buttons = dialogButtons(dialog);
            buttons->button(QDialogButtonBox::Ok)->setText(prepared.savesChanges ? "保存并切换" : "切换方案");
            buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
            const auto answer = dialog.exec();
            if (!guard) return;
            if (answer != QDialog::Accepted) { m_busy = false; m_task = 0; updateActions(); return; }
            m_task = m_service->switchBranch(prepared, this, [this, id](const OperationResult &result)
            { complete(result); if (result.success) emit navigate({PageId::History, id}); });
        });
    });
}

void HomePageBranchesPage::showOperations()
{
    const auto branch = selected(); if (branch.ref.isEmpty()) return;
    const auto r = request(branch);
    const QPointer<HomePageBranchesPage> guard(this);
    QMenu menu(this);
    auto *compareAction = menu.addAction("与其他方案比较…");
    QAction *mergeAction = nullptr, *renameAction = nullptr, *upstreamAction = nullptr, *pushAction = nullptr, *publishAction = nullptr;
    if (!branch.remoteBranch)
    {
        mergeAction = menu.addAction("合并到正在使用的方案…"); mergeAction->setEnabled(!branch.current);
        renameAction = menu.addAction("重命名…");
        upstreamAction = menu.addAction("设置对应云端…");
        pushAction = menu.addAction("上传此方案的已保存版本");
        publishAction = menu.addAction("发布为同名云端方案…");
    }
    menu.addSeparator();
    auto *removeAction = menu.addAction(branch.remoteBranch ? "删除云端方案…" : "删除本地方案…"); removeAction->setEnabled(!branch.current);
    auto *forceAction = branch.remoteBranch ? nullptr : menu.addAction("仍然删除未合并方案…");
    if (forceAction) forceAction->setEnabled(!branch.current);
    const auto chosen = menu.exec(ui->operations->mapToGlobal(QPoint(0, ui->operations->height())));
    if (!guard || !chosen) return;
    if (chosen == compareAction) { compare(branch); return; }
    if (chosen == mergeAction) { merge(branch); return; }
    if (chosen == renameAction)
    {
        bool ok = false; const auto name = UiDialog::getText(dialogOwner(), "重命名方案", "新名称（云端名称不会自动修改）", branch.name, &ok);
        if (!guard || !ok) return;
        auto renamed = r; renamed.name = name; m_busy = true; updateActions();
        m_task = m_service->renameBranch(renamed, this, [this](const OperationResult &result) { complete(result); }); return;
    }
    if (chosen == upstreamAction)
    {
        UiDialog::Dialog dialog(dialogOwner(), "branchUpstreamDialog", "设置对应云端");
        auto *combo = new QComboBox(&dialog); combo->addItem("不关联云端", QString());
        for (const auto &b : m_snapshot.branches) if (b.remoteBranch) combo->addItem(b.name, b.ref);
        combo->setCurrentIndex(qMax(0, combo->findData(branch.upstream))); dialog.contentLayout()->addWidget(combo);
        dialogButtons(dialog);
        if (dialog.exec() != QDialog::Accepted || !guard) return;
        auto linked = r; linked.upstream = combo->currentData().toString(); m_busy = true; updateActions();
        m_task = m_service->setBranchUpstream(linked, this, [this](const OperationResult &result) { complete(result); }); return;
    }
    if (chosen == pushAction || chosen == publishAction)
    {
        auto upload = r;
        if (chosen == publishAction)
        {
            if (!UiDialog::confirm(dialogOwner(), "将已保存版本发布为云端方案“" + branch.name + "”？旧的云端方案会保留。", "发布方案") || !guard) return;
            upload.name = branch.name;
        }
        m_busy = true; updateActions();
        m_task = m_service->uploadBranch(upload, this, [this](const OperationResult &result) { complete(result); }); return;
    }
    const bool force = chosen == forceAction;
    const auto text = branch.remoteBranch ? QString("删除云端方案“%1”？本地方案仍会保留。云端已变化或服务器拒绝时将停止。").arg(branch.name)
        : force ? QString("仍然删除“%1”？尚未合并的独有版本可能不再可恢复。其他本地方案和云端分支不受影响。").arg(branch.name)
                : QString("删除本地方案“%1”？Git 会检查是否已合并；尚未合并时会停止。云端分支不受影响。").arg(branch.name);
    if (!UiDialog::confirm(dialogOwner(), text, "删除方案") || !guard) return;
    auto removed = r; removed.force = force; m_busy = true; updateActions();
    const auto done = [this](const OperationResult &result) { complete(result); };
    m_task = branch.remoteBranch ? m_service->deleteRemoteBranch(removed, this, done) : m_service->deleteBranch(removed, this, done);
}
void HomePageBranchesPage::compare(const BranchInfo &branch)
{
    const QPointer<HomePageBranchesPage> guard(this);
    const auto id = m_id;
    UiDialog::Dialog dialog(dialogOwner(), "compareBranchesDialog", "比较方案");
    auto *label = new QLabel("修改前：" + branch.name + "\n选择修改后版本：", &dialog); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); dialog.contentLayout()->addWidget(label);
    auto *combo = new QComboBox(&dialog);
    for (const auto &b : m_snapshot.branches) if (b.ref != branch.ref) combo->addItem(b.name, b.head);
    dialog.contentLayout()->addWidget(combo); dialogButtons(dialog);
    if (!combo->count() || dialog.exec() != QDialog::Accepted || !guard || id != m_id) return;
    Route route{PageId::Diff, id, combo->currentData().toString()}; route.oldCommit = branch.head; emit navigate(route);
}
void HomePageBranchesPage::merge(const BranchInfo &branch)
{
    const auto r = request(branch);
    const QPointer<HomePageBranchesPage> guard(this);
    if (!UiDialog::confirm(dialogOwner(), QString("将“%1”合并到正在使用的“%2”？\n接下来会分析差异，预览确认后才应用到源文件。来源方案会保留。").arg(branch.name, r.context.ref.mid(11)), "分析合并") || !guard) return;
    m_busy = true; updateActions();
    m_task = m_service->prepareBranchMerge(r, this, [this, id = r.context.id](const BackupResult<SyncResolutionSession> &result)
    { complete(result.result); if (result.result.success && !result.value.id.isEmpty()) emit navigate({PageId::Conflict, id}); });
}
