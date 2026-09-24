#include "homepage.h"
#include "ElaMessageBar.h"
#include "ElaPushButton.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

HomePage::HomePage(Backup::BackupService *service, QWidget *parent) : QWidget(parent), m_service(service)
{
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(16);
    auto heading = new QLabel(tr("文件备份"), this);
    auto font = heading->font();
    font.setPointSize(22);
    font.setBold(true);
    heading->setFont(font);
    layout->addWidget(heading);
    auto button = [this](const QString &text, QHBoxLayout *row, std::function<void()> handler)
    {
        auto b = new ElaPushButton(text, this);
        row->addWidget(b);
        connect(b, &QPushButton::clicked, this, std::move(handler));
        return b;
    };
    auto tools = new QHBoxLayout;
    button(tr("添加文件"), tools, [this]
           {
               addLocal(false);
           });
    button(tr("添加文件夹"), tools, [this]
           {
               addLocal(true);
           });
    button(tr("从云端导入"), tools, [this]
           {
               importRemote();
           });
    tools->addStretch();
    layout->addLayout(tools);
    auto split = new QSplitter(this);
    m_objects = new QListWidget(split);
    m_objects->setMinimumWidth(190);
    m_objects->setMaximumWidth(340);
    auto detail = new QWidget(split);
    auto body = new QVBoxLayout(detail);
    m_title = new QLabel(tr("选择一个备份对象"), detail);
    font.setPointSize(17);
    m_title->setFont(font);
    body->addWidget(m_title);
    m_source = new QLabel(detail);
    m_source->setWordWrap(true);
    m_source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(m_source);
    m_status = new QLabel(detail);
    m_status->setWordWrap(true);
    body->addWidget(m_status);
    auto actions = new QHBoxLayout;
    button(tr("立即备份"), actions, [this]
           {
               action("snapshot");
           });
    button(tr("打开源文件"), actions, [this]
           {
               for (const auto &t : m_service->trackings())
                   if (t.id == m_selected)
                       QDesktopServices::openUrl(QUrl::fromLocalFile(t.source));
           });
    button(tr("重试"), actions, [this]
           {
               m_service->retry(m_selected);
           });
    button(tr("取消任务"), actions, [this]
           {
               m_service->cancel(m_selected);
           });
    actions->addStretch();
    body->addLayout(actions);
    m_tabs = new QTabWidget(detail);
    body->addWidget(m_tabs, 1);
    auto historyPage = new QWidget(m_tabs);
    auto historyLayout = new QVBoxLayout(historyPage);
    m_history = new QTableWidget(0, 4, historyPage);
    m_history->setHorizontalHeaderLabels({tr("时间"), tr("版本"), tr("操作"), tr("备注")});
    m_history->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_history->setSelectionMode(QAbstractItemView::SingleSelection);
    m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_history->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_history->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_history->verticalHeader()->hide();
    historyLayout->addWidget(m_history, 1);
    auto historyActions = new QHBoxLayout;
    button(tr("预览"), historyActions, [this]
           {
               action("export");
           });
    button(tr("恢复"), historyActions, [this]
           {
               const auto revision = selectedRevision(), id = m_selected;
               if (!revision.isEmpty() && QMessageBox::question(window(), tr("恢复历史版本"), tr("将先保存当前修改，再恢复版本 %1。继续吗？").arg(revision.left(12))) == QMessageBox::Yes)
               {
                   Backup::Request request;
                   request.trackingId = id;
                   request.operation = "restore";
                   request.revision = revision;
                   send(request);
               }
           });
    button(tr("对比"), historyActions, [this]
           {
               action("diff");
           });
    button(tr("编辑备注"), historyActions, [this]
           {
               const auto revision = selectedRevision();
               if (revision.isEmpty())
                   return;
               bool ok = false;
               const auto text = QInputDialog::getMultiLineText(window(), tr("编辑备注"), tr("备注不会修改历史版本"), {}, &ok);
               if (ok)
               {
                   Backup::Request r;
                   r.operation = "annotate";
                   r.revision = revision;
                   r.text = text;
                   send(r, [this](const auto &result)
                        {
                            if (result.success)
                                loadHistory();
                        });
               }
           });
    button(tr("生成 AI 备注"), historyActions, [this]
           {
               if (!selectedRevision().isEmpty())
                   m_service->requestAiAnnotation(m_selected, selectedRevision());
           });
    historyActions->addStretch();
    historyLayout->addLayout(historyActions);
    auto pagination = new QHBoxLayout;
    button(tr("刷新"), pagination, [this]
           {
               loadHistory();
           });
    m_more = button(tr("加载更多"), pagination, [this]
                    {
                        loadHistory(true);
                    });
    pagination->addStretch();
    historyLayout->addLayout(pagination);
    m_tabs->addTab(historyPage, tr("历史版本"));
    m_diff = new QTextEdit(m_tabs);
    m_diff->setReadOnly(true);
    m_diff->setLineWrapMode(QTextEdit::NoWrap);
    m_tabs->addTab(m_diff, tr("版本对比"));
    auto cloud = new QWidget(m_tabs);
    auto cloudLayout = new QVBoxLayout(cloud);
    cloudLayout->addWidget(new QLabel(tr("云端仓库地址"), cloud));
    m_remote = new QLineEdit(cloud);
    cloudLayout->addWidget(m_remote);
    auto remoteActions = new QHBoxLayout;
    button(tr("保存地址"), remoteActions, [this]
           {
               Backup::Request r;
               r.operation = "setRemote";
               r.remote = m_remote->text().trimmed();
               send(r);
           });
    button(tr("拉取并更新源文件"), remoteActions, [this]
           {
               action("pull");
           });
    button(tr("推送"), remoteActions, [this]
           {
               action("push");
           });
    remoteActions->addStretch();
    cloudLayout->addLayout(remoteActions);
    auto hint = new QLabel(tr("拉取前会保存本地修改。历史发生分叉时保留双方版本，等待冲突解决。"), cloud);
    hint->setWordWrap(true);
    cloudLayout->addWidget(hint);
    auto conflictActions = new QHBoxLayout;
    button(tr("查看冲突"), conflictActions, [this]
           {
               const auto c = m_service->queryConflict(m_selected);
               QMessageBox::information(window(), tr("同步冲突"), c.category.isEmpty() ? tr("当前没有待处理冲突") : tr("类型：%1\n本地：%2\n远端：%3\n\n双方版本均已保留。冲突解决系统尚未实现。").arg(c.category, c.local, c.remote));
           });
    button(tr("覆盖远端…"), conflictActions, [this]
           {
               Backup::Request r;
               r.operation = "remoteHead";
               const auto id = m_selected;
               send(r, [this, id](const auto &result)
                    {
                        if (!result.success)
                            return;
                        if (QMessageBox::warning(window(), tr("覆盖远端历史"), tr("远端当前版本：%1\n\n将用本地历史替换远端 main。确认覆盖？").arg(result.revision.left(12)), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
                        {
                            Backup::Request replace;
                            replace.trackingId = id;
                            replace.operation = "resetRemote";
                            replace.revision = result.revision;
                            send(replace);
                        }
                    });
           });
    conflictActions->addStretch();
    cloudLayout->addLayout(conflictActions);
    cloudLayout->addStretch();
    m_tabs->addTab(cloud, tr("云端同步"));
    auto manage = new QHBoxLayout;
    button(tr("重建备份…"), manage, [this]
           {
               if (!m_selected.isEmpty() && QMessageBox::warning(window(), tr("重建备份"), tr("本地历史将替换为当前源文件的单一快照。远端不会自动覆盖。继续吗？"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
                   action("rebuild");
           });
    button(tr("删除备份…"), manage, [this]
           {
               if (!m_selected.isEmpty() && QMessageBox::warning(window(), tr("删除备份"), tr("将删除本地备份和全部历史，源文件及远端保持不变。继续吗？"), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
                   action("remove");
           });
    manage->addStretch();
    body->addLayout(manage);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    connect(m_objects, &QListWidget::currentRowChanged, this, &HomePage::selectObject);
    connect(service, &Backup::BackupService::trackingChanged, this, &HomePage::refreshObjects);
    connect(service, &Backup::BackupService::stateChanged, this, [this](const QString &id, const QString &channel, const QString &state)
            {
                m_states[id][channel] = state;
                if (id == m_selected)
                    renderState();
            });
    connect(service, &Backup::BackupService::startupFailed, this, [this](const QString &message)
            {
                notify(message);
            });
    connect(service, &Backup::BackupService::taskFinished, this, [this](const Backup::TaskResult &result)
            {
                if (m_callbacks.contains(result.requestId))
                {
                    const auto callback = m_callbacks.take(result.requestId);
                    callback(result);
                }
                if (!result.success && result.code != Backup::ErrorCode::Cancelled && !result.retryable && result.operation != "diff")
                    notify(result.message);
                else if (!result.message.isEmpty() && result.success)
                    notify(result.message, false);
                if (result.success && result.changed && result.trackingId == m_selected)
                    loadHistory();
            });
    refreshObjects();
}
void HomePage::notify(const QString &message, bool error)
{
    if (error)
        ElaMessageBar::error(ElaMessageBarType::BottomRight, tr("操作未完成"), message, 6000, window());
    else
        ElaMessageBar::success(ElaMessageBarType::BottomRight, tr("备份提示"), message, 5000, window());
}
void HomePage::refreshObjects()
{
    const auto selected = m_selected;
    m_objects->blockSignals(true);
    m_objects->clear();
    int row = -1;
    for (const auto &tracking : m_service->trackings())
    {
        auto item = new QListWidgetItem(QFileInfo(tracking.source).fileName(), m_objects);
        item->setData(Qt::UserRole, tracking.id);
        item->setToolTip(tracking.source);
        if (tracking.id == selected)
            row = m_objects->count() - 1;
    }
    m_objects->blockSignals(false);
    m_objects->setCurrentRow(row >= 0 ? row : (m_objects->count() ? 0 : -1));
    if (!m_objects->count())
    {
        m_selected.clear();
        selectObject();
    }
}
void HomePage::selectObject()
{
    const auto item = m_objects->currentItem();
    m_selected = item ? item->data(Qt::UserRole).toString() : QString();
    m_history->setRowCount(0);
    m_diff->clear();
    m_remote->clear();
    m_more->setEnabled(false);
    m_tabs->setEnabled(!m_selected.isEmpty());
    m_title->setText(tr("选择一个备份对象"));
    m_source->clear();
    for (const auto &t : m_service->trackings())
        if (t.id == m_selected)
        {
            m_title->setText(QFileInfo(t.source).fileName());
            m_source->setText(t.source);
            m_remote->setText(t.remote);
        }
    renderState();
    if (!m_selected.isEmpty())
        loadHistory();
}
void HomePage::renderState()
{
    const auto states = m_states.value(m_selected);
    m_status->setText(tr("备份：%1\n同步：%2    AI：%3").arg(states.value("backup", tr("等待首次备份")), states.value("sync", tr("未运行")), states.value("ai", tr("未运行"))));
}
void HomePage::send(Backup::Request request, std::function<void(const Backup::TaskResult &)> callback)
{
    if (request.trackingId.isEmpty())
        request.trackingId = m_selected;
    if (request.trackingId.isEmpty())
        return;
    request.requestId = Backup::newId();
    if (callback)
        m_callbacks.insert(request.requestId, std::move(callback));
    if (m_service->submit(request).isEmpty())
        m_callbacks.remove(request.requestId);
}
void HomePage::loadHistory(bool append)
{
    if (m_selected.isEmpty())
        return;
    const auto id = m_selected;
    const auto generation = append ? m_historyGeneration : ++m_historyGeneration;
    m_more->setEnabled(false);
    const auto offset = append ? m_history->rowCount() : 0;
    Backup::Request request;
    request.operation = "history";
    request.offset = offset;
    send(request, [this, id, offset, generation](const auto &result)
         {
             if (id != m_selected || generation != m_historyGeneration || !result.success)
                 return;
             if (!offset)
                 m_history->setRowCount(0);
             for (const auto &value : result.data["rows"].toArray())
             {
                 const auto row = value.toObject();
                 const int index = m_history->rowCount();
                 m_history->insertRow(index);
                 const auto annotation = row["annotation"].toObject();
                 const auto note = annotation["manual"].toString() + (annotation["ai"].toString().isEmpty() ? QString() : "\nAI：" + annotation["ai"].toString());
                 QStringList cells{QDateTime::fromSecsSinceEpoch(row["time"].toString().toLongLong()).toString("MM-dd HH:mm:ss"), row["id"].toString().left(10), row["summary"].toString().section(" · ", 0, 0), note.trimmed()};
                 for (int col = 0; col < cells.size(); ++col)
                 {
                     auto item = new QTableWidgetItem(cells[col]);
                     item->setData(Qt::UserRole, row["id"].toString());
                     item->setToolTip(col == 1 ? row["id"].toString() : cells[col]);
                     m_history->setItem(index, col, item);
                 }
             }
             m_more->setEnabled(result.data["hasMore"].toBool());
         });
}
QString HomePage::selectedRevision() const
{
    const auto row = m_history->currentRow();
    return row < 0 ? QString() : m_history->item(row, 0)->data(Qt::UserRole).toString();
}
void HomePage::action(const QString &operation)
{
    Backup::Request request;
    request.operation = operation;
    request.fullScan = true;
    if (operation == "export" || operation == "restore" || operation == "diff")
    {
        request.revision = selectedRevision();
        if (request.revision.isEmpty())
        {
            notify(tr("请先选择一个历史版本"), false);
            return;
        }
    }
    const auto id = m_selected;
    send(request, [this, operation, id](const auto &result)
         {
             if (!result.success)
                 return;
             if (operation == "export")
                 QDesktopServices::openUrl(QUrl::fromLocalFile(result.data["path"].toString()));
             if (operation == "diff" && id == m_selected)
             {
                 m_diff->setPlainText(result.data["text"].toString());
                 m_tabs->setCurrentIndex(1);
             }
         });
}
void HomePage::addLocal(bool directory)
{
    const auto source = directory ? QFileDialog::getExistingDirectory(window(), tr("选择文件夹")) : QFileDialog::getOpenFileName(window(), tr("选择文件"));
    if (source.isEmpty())
        return;
    try
    {
        m_selected = m_service->track(source);
        refreshObjects();
    }
    catch (const Backup::Error &e)
    {
        notify(e.message);
    }
}
void HomePage::importRemote()
{
    bool ok = false;
    const auto remote = QInputDialog::getText(window(), tr("导入云端备份"), tr("新版备份仓库地址"), QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || remote.isEmpty())
        return;
    const auto parent = QFileDialog::getExistingDirectory(window(), tr("选择源文件的保存目录"));
    if (parent.isEmpty())
        return;
    const auto name = QInputDialog::getText(window(), tr("源文件名称"), tr("输入新文件或文件夹名称；目标必须尚不存在"), QLineEdit::Normal, "imported", &ok);
    if (!ok || name.isEmpty() || name.contains('/') || name.contains('\\') || name == "." || name == "..")
        return;
    try
    {
        m_selected = m_service->track(QDir(parent).filePath(name), remote, true);
        refreshObjects();
    }
    catch (const Backup::Error &e)
    {
        notify(e.message);
    }
}
