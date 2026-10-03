#include "backup_engine.h"
#include <QSet>

namespace
{
OperationResult changedTags() { return OperationResult::warn("标记已变化", "请重新打开版本菜单后再试。"); }
}
OperationResult BackupEngine::recoverTags(BackupRecord record)
{
    auto git = repository(record.id);
    auto valid = git.validate();
    if (!valid.success) return valid;
    const auto refs = git.tagRefs();
    if (!refs.result.success) return refs.result;
    bool before = true, after = true;
    for (const auto &change : record.tagJournal)
    {
        before &= refs.value.value(change.name) == change.before;
        after &= refs.value.value(change.name) == change.after;
    }
    if (!after)
    {
        if (!before) return OperationResult::warn("重要版本需要检查", "未完成的标记操作遇到外部修改，已保留操作记录与所有文件。");
        const auto applied = git.updateTags(record.tagJournal);
        if (!applied.success) return applied;
    }
    record.tagJournal.clear();
    ++record.tagRevision;
    return m_catalog.save(record);
}
OperationResult BackupEngine::commitTagChanges(BackupRecord record, const QVector<TagRefChange> &changes)
{
    const auto *found = m_catalog.find(record.id);
    if (!found || !found->tagJournal.isEmpty()) return changedTags();
    const auto before = *found;
    record.tagJournal = changes;
    ++record.tagRevision;
    auto saved = m_catalog.save(record);
    if (!saved.success) return saved;
    const auto applied = repository(record.id).updateTags(changes);
    if (!applied.success)
    {
        // update-ref commits all refs together. A failed transaction changed none.
        const auto reset = m_catalog.save(before);
        auto result = applied;
        if (!reset.success) result.warning = "操作记录已保留，下次打开时将核对标记状态。";
        return result;
    }
    record.tagJournal.clear();
    saved = m_catalog.save(record);
    if (!saved.success) saved.warning = "标记已保存，操作记录将在下次打开时核对。";
    return saved;
}
OperationResult BackupEngine::changeTag(const TagRequest &request, const QString &newName, bool create)
{
    const auto checked = require(request.id);
    if (!checked.result.success) return checked.result;
    auto record = checked.value;
    if (record.generation != request.generation || !record.tagJournal.isEmpty()) return changedTags();
    if (record.state != BackupSyncState::Tracking)
        return OperationResult::warn("暂时无法修改标记", "请先处理此备份的同步差异或检查状态。");
    auto git = repository(request.id);
    const auto commit = git.resolve(request.commit);
    if (!commit.result.success || commit.value != request.commit) return changedTags();
    const auto marked = git.tags();
    const auto refs = git.tagRefs();
    if (!marked.result.success) return marked.result;
    if (!refs.result.success) return refs.result;
    const auto name = newName.trimmed();
    if (create || !name.isEmpty())
    {
        const auto valid = git.validateTagName(name);
        if (!valid.success) return valid;
        if (refs.value.contains(name) && (create || name != request.name))
            return OperationResult::fail("名称已使用", "请换一个名称，每个重要版本名称只能使用一次。");
    }
    if (create)
    {
        for (const auto &tag : marked.value)
            if (tag.commitOid == request.commit) return OperationResult::warn("此版本已有标记", "请打开“管理重要版本”修改名称。");
    }
    else
    {
        bool matched = false;
        for (const auto &tag : marked.value)
            matched |= tag.name == request.name && tag.refOid == request.expectedOid && tag.commitOid == request.commit;
        if (!matched) return changedTags();
        if (name == request.name) return OperationResult::ok("名称未变");
    }
    const auto endpoint = git.tagEndpoint();
    if (!endpoint.result.success) return endpoint.result;
    QVector<TagRefChange> changes;
    if (!create) changes.append({request.name, request.expectedOid, {}});
    if (!name.isEmpty()) changes.append({name, {}, create ? request.commit : request.expectedOid});
    if (!endpoint.value.isEmpty())
    {
        auto &remote = record.tagRemotes[endpoint.value];
        for (const auto &change : changes)
        {
            remote.pending[change.name] = change.after;
            remote.conflicts.remove(change.name);
            if (remote.base.value(change.name) == change.after) remote.pending.remove(change.name);
        }
    }
    const auto saved = commitTagChanges(record, changes);
    if (!saved.success) return saved;
    return OperationResult::ok(name.isEmpty() ? "已取消标记" : create ? "已标记为重要版本" : "名称已更新",
        name.isEmpty() ? "历史版本和文件仍然保留。" + (endpoint.value.isEmpty() ? QString() : "下次上传时同步。")
                       : endpoint.value.isEmpty() ? QString() : "下次上传时同步。");
}
OperationResult BackupEngine::createTag(const TagRequest &request) { return changeTag(request, request.name, true); }
OperationResult BackupEngine::renameTag(const TagRequest &request, const QString &name)
{
    if (name.trimmed().isEmpty()) return OperationResult::fail("名称不可用", "请输入重要版本名称。");
    return changeTag(request, name, false);
}
OperationResult BackupEngine::removeTag(const TagRequest &request) { return changeTag(request, {}, false); }
