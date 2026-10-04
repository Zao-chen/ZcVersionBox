#include "homepage_page_backup.h"
#include "windows/mainwindow_dialog.h"
#include "windows/mainwindow_presentation.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

void HomePageBackupPage::trackTagDialog(UiDialog::Dialog *dialog)
{
    m_tagDialog = dialog;
    m_editing = true;
    dialog->prepareAsync();
    connect(dialog, &QDialog::finished, this, [this]
    {
        m_tagDialog = nullptr;
        m_editing = false;
        m_refreshPending = false;
        if (isVisible()) refresh();
    });
    dialog->open();
}
void HomePageBackupPage::manageTags(const RevisionContext &context)
{
    if (!isCurrentContext(context) || m_editing) return;
    const auto tags = revisionTags(context);
    auto *dialog = new UiDialog::Dialog(this, QStringLiteral("versionTagDialog"),
                                        tags.isEmpty() ? QStringLiteral("标记为里程碑版本") : QStringLiteral("管理里程碑版本"));
    auto *layout = dialog->contentLayout();
    auto *selector = new QComboBox(dialog);
    selector->setObjectName("versionTagSelector");
    selector->setAccessibleName("选择里程碑标记");
    for (const auto &tag : tags) selector->addItem(tag.name);
    selector->setVisible(tags.size() > 1);
    layout->addWidget(selector);
    auto *label = new QLabel("名称", dialog);
    UiStyle::text(label);
    auto *name = new QLineEdit(dialog);
    name->setObjectName("versionTagName"); name->setAccessibleName("里程碑名称");
    name->setPlaceholderText("如：交稿版"); label->setBuddy(name);
    if (!tags.isEmpty()) name->setText(tags.first().name);
    connect(selector, &QComboBox::currentIndexChanged, dialog, [name, tags](int index)
    { if (index >= 0 && index < tags.size()) { name->setText(tags[index].name); name->selectAll(); } });
    layout->addWidget(label); layout->addWidget(name);
    auto *error = new QLabel(dialog);
    error->setObjectName("versionTagError"); error->setTextFormat(Qt::PlainText); error->setWordWrap(true);
    UiStyle::text(error, UiStyle::FontRole::Caption, true); error->hide(); layout->addWidget(error);
    auto *buttons = dialog->buttonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setText("保存");
    buttons->button(QDialogButtonBox::Save)->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    QPushButton *remove = nullptr;
    if (!tags.isEmpty())
    {
        remove = new QPushButton("取消标记，保留版本", dialog);
        remove->setObjectName("removeVersionTagButton"); remove->setAutoDefault(false);
    }
    auto *footer = dialog->footerLayout();
    if (remove)
        footer->addWidget(remove);
    footer->addStretch();
    footer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const auto submit = [this, context, tags, dialog, name, selector, error, buttons, remove](bool deleting)
    {
        if (!isCurrentContext(context)) { dialog->reject(); return; }
        if (!deleting && name->text().trimmed().isEmpty()) { error->setText("请输入里程碑名称。"); error->show(); name->setFocus(); return; }
        TagRequest request{context.backupId, context.commit, context.repositoryGeneration, name->text().trimmed(), {}};
        if (!tags.isEmpty())
        {
            const auto &tag = tags[selector->currentIndex()];
            request.name = tag.name; request.expectedOid = tag.refOid;
        }
        buttons->button(QDialogButtonBox::Save)->setEnabled(false);
        name->setEnabled(false); selector->setEnabled(false); if (remove) remove->setEnabled(false);
        const auto done = [this, context, dialog, name, selector, error, buttons, remove](const OperationResult &result)
        {
            if (!isCurrentContext(context)) { dialog->reject(); return; }
            if (result.success) { emit notification(result); dialog->accept(); }
            else
            {
                error->setText(result.message + (result.warning.isEmpty() ? QString() : "\n" + result.warning)); error->show();
                buttons->button(QDialogButtonBox::Save)->setEnabled(true);
                name->setEnabled(true); selector->setEnabled(true); if (remove) remove->setEnabled(true);
                name->setFocus(); name->selectAll();
            }
        };
        if (deleting) m_service->removeTag(request, dialog, done);
        else if (tags.isEmpty()) m_service->createTag(request, dialog, done);
        else m_service->renameTag(request, name->text(), dialog, done);
    };
    connect(buttons, &QDialogButtonBox::accepted, dialog, [submit] { submit(false); });
    if (remove) connect(remove, &QPushButton::clicked, dialog, [submit] { submit(true); });
    trackTagDialog(dialog);
    name->setFocus(); name->selectAll();
}
void HomePageBackupPage::resolveTagConflicts()
{
    if (m_editing) return;
    const auto conflicts = m_service->tagConflicts(m_id);
    if (conflicts.isEmpty()) return;
    const auto id = m_id;
    const auto generation = m_service->repositoryGeneration(id), pageGeneration = m_contextGeneration;
    const auto conflict = conflicts.first();
    auto *dialog = new UiDialog::Dialog(this, QStringLiteral("versionTagConflictDialog"), QStringLiteral("处理里程碑标记"));
    auto *layout = dialog->contentLayout();
    auto *description = new QLabel(conflict.remoteOid.isEmpty() ? QString("云端已取消“%1”。可以保留本地标记并换一个名字，或使用云端的结果。").arg(conflict.name)
        : QString("云端的“%1”指向另一个版本。可以给本地标记换个名字以保留两份，或使用云端标记。历史内容会保留。").arg(conflict.name), dialog);
    description->setTextFormat(Qt::PlainText); description->setWordWrap(true); UiStyle::text(description); layout->addWidget(description);
    auto *name = new QLineEdit(conflict.name + "-本机", dialog);
    name->setObjectName("conflictTagName"); name->setAccessibleName("本地标记的新名称"); layout->addWidget(name);
    auto *error = new QLabel(dialog); error->setTextFormat(Qt::PlainText); error->setWordWrap(true); UiStyle::text(error, UiStyle::FontRole::Caption, true); error->hide(); layout->addWidget(error);
    auto *buttons = dialog->buttonBox(QDialogButtonBox::Cancel);
    auto *keep = buttons->addButton("给本地换个名字", QDialogButtonBox::ActionRole);
    auto *cloud = buttons->addButton("使用云端", QDialogButtonBox::ActionRole);
    keep->setObjectName("keepBothTagsButton"); cloud->setObjectName("useRemoteTagButton"); keep->setDefault(true);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    auto *footer = dialog->footerLayout();
    footer->addStretch();
    footer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const auto submit = [this, id, generation, pageGeneration, conflict, dialog, name, error, keep, cloud](bool both)
    {
        if (id != m_id || pageGeneration != m_contextGeneration || generation != m_service->repositoryGeneration(id)) { dialog->reject(); return; }
        if (both && name->text().trimmed().isEmpty()) { error->setText("请输入本地标记的新名称。"); error->show(); return; }
        keep->setEnabled(false); cloud->setEnabled(false); name->setEnabled(false);
        m_service->resolveTagConflict(id, conflict, both ? name->text() : QString(), dialog,
            [this, id, generation, pageGeneration, dialog, name, error, keep, cloud](const OperationResult &result)
        {
            if (id != m_id || pageGeneration != m_contextGeneration || generation != m_service->repositoryGeneration(id)) { dialog->reject(); return; }
            if (result.success)
            {
                emit notification(result); dialog->accept();
                QTimer::singleShot(0, this, [this, id] { if (isVisible() && id == m_id) resolveTagConflicts(); });
            }
            else { error->setText(result.message); error->show(); keep->setEnabled(true); cloud->setEnabled(true); name->setEnabled(true); }
        });
    };
    connect(keep, &QPushButton::clicked, dialog, [submit] { submit(true); });
    connect(cloud, &QPushButton::clicked, dialog, [submit] { submit(false); });
    trackTagDialog(dialog); name->setFocus(); name->selectAll();
}
