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
        record.tagEndpoint = endpoint.value;
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

OperationResult BackupEngine::syncTags(const QString &id, bool push)
{
    auto checked = require(id);
    if (!checked.result.success) return checked.result;
    auto record = checked.value;
    if (!record.tagJournal.isEmpty()) return changedTags();
    auto git = repository(id);
    const auto endpoint = git.tagEndpoint();
    if (!endpoint.result.success) return endpoint.result;
    const auto fetched = git.fetchTags();
    if (!fetched.result.success) return fetched.result;
    const auto local = git.tagRefs();
    if (!local.result.success) return local.result;
    record.tagEndpoint = endpoint.value;
    auto &state = record.tagRemotes[endpoint.value];
    QSet<QString> names;
    for (const QMap<QString, QString> *map : {&local.value, &fetched.value, static_cast<const QMap<QString, QString> *>(&state.base), static_cast<const QMap<QString, QString> *>(&state.pending)})
        for (auto it = map->cbegin(); it != map->cend(); ++it) names.insert(it.key());
    QVector<TagRefChange> changes;
    QStringList retained;
    for (const auto &name : names)
    {
        const auto ours = local.value.value(name), theirs = fetched.value.value(name), base = state.base.value(name);
        auto desired = ours;
        if (!state.pending.contains(name) && !ours.isEmpty() && ours != base && ours != theirs)
            state.pending[name] = ours;
        if (state.pending.contains(name))
        {
            const auto pending = state.pending.value(name);
            // External edits after the UI operation must not be overwritten by its queued upload.
            if (pending != ours)
                return OperationResult::warn("重要版本已变化", "本地标记在应用外被修改，请恢复该标记后重试同步：" + name);
            if (pending == theirs)
            {
                state.pending.remove(name); state.conflicts.remove(name);
                if (theirs.isEmpty()) state.base.remove(name); else state.base[name] = theirs;
            }
            else if (theirs != base)
            {
                if (pending.isEmpty())
                {
                    desired = theirs; state.pending.remove(name); state.conflicts.remove(name);
                    if (theirs.isEmpty()) state.base.remove(name); else state.base[name] = theirs;
                    retained.append(name);
                }
                else state.conflicts[name] = theirs;
            }
            else state.conflicts.remove(name);
        }
        else
        {
            desired = theirs;
            state.conflicts.remove(name);
            if (theirs.isEmpty()) state.base.remove(name); else state.base[name] = theirs;
        }
        if (ours != desired) changes.append({name, ours, desired});
    }
    auto saved = commitTagChanges(record, changes);
    if (!saved.success) return saved;
    if (!state.conflicts.isEmpty())
        return OperationResult::warn("重要版本需要处理", "云端存在不同的同名标记。本地与云端内容均已保留，请在历史版本中处理标记。");
    if (push && !state.pending.isEmpty())
    {
        const auto remote = git.remoteName();
        if (!remote.result.success) return remote.result;
        QStringList args{"push", "--atomic", "--no-follow-tags"};
        QStringList refs;
        for (auto it = state.pending.cbegin(); it != state.pending.cend(); ++it)
        {
            const auto valid = git.validateTagName(it.key()); if (!valid.success) return valid;
            args << "--force-with-lease=refs/tags/" + it.key() + ':' + state.base.value(it.key());
            refs << it.value() + ":refs/tags/" + it.key();
        }
        args << "--" << remote.value; args += refs;
        const auto uploaded = git.run(args, {}, true, 300000);
        if (!uploaded.success()) return GitRepository::outcome(uploaded, "重要版本尚未同步");
        for (auto it = state.pending.cbegin(); it != state.pending.cend(); ++it)
            if (it.value().isEmpty()) state.base.remove(it.key()); else state.base[it.key()] = it.value();
        state.pending.clear();
        ++record.tagRevision;
        saved = m_catalog.save(record);
        if (!saved.success) return saved;
    }
    auto result = OperationResult::ok("重要版本已同步");
    if (!retained.isEmpty()) result.warning = "取消标记期间云端已有修改，已保留云端标记，请重新确认：" + retained.join("、");
    return result;
}
OperationResult BackupEngine::resolveTagConflict(const QString &id, const TagConflict &conflict, const QString &newName)
{
    const auto checked = require(id);
    if (!checked.result.success) return checked.result;
    auto record = checked.value;
    auto git = repository(id);
    const auto endpoint = git.tagEndpoint();
    if (!endpoint.result.success) return endpoint.result;
    if (endpoint.value != conflict.endpoint || !record.tagJournal.isEmpty()) return changedTags();
    auto &state = record.tagRemotes[endpoint.value];
    if (!state.conflicts.contains(conflict.name) || state.conflicts.value(conflict.name) != conflict.remoteOid ||
        state.pending.value(conflict.name) != conflict.localOid) return changedTags();
    const auto fetched = git.fetchTags(), local = git.tagRefs();
    if (!fetched.result.success) return fetched.result;
    if (!local.result.success) return local.result;
    if (fetched.value.value(conflict.name) != conflict.remoteOid || local.value.value(conflict.name) != conflict.localOid) return changedTags();
    const auto name = newName.trimmed();
    QVector<TagRefChange> changes;
    if (!name.isEmpty())
    {
        const auto valid = git.validateTagName(name); if (!valid.success) return valid;
        if (local.value.contains(name) || fetched.value.contains(name)) return OperationResult::fail("名称已使用", "请换一个名称。");
        changes.append({name, {}, conflict.localOid});
        state.pending[name] = conflict.localOid;
        state.base.remove(name);
    }
    changes.append({conflict.name, conflict.localOid, conflict.remoteOid});
    state.pending.remove(conflict.name); state.conflicts.remove(conflict.name);
    if (conflict.remoteOid.isEmpty()) state.base.remove(conflict.name); else state.base[conflict.name] = conflict.remoteOid;
    auto result = commitTagChanges(record, changes);
    return result.success ? OperationResult::ok("标记已处理", name.isEmpty() ? "已使用云端标记，历史版本仍然保留。" : "已保留两份标记，下次上传时同步。") : result;
}
BackupResult<PreparedRebuild> BackupEngine::prepareRebuild(const QString &id)
{
    const auto checked = require(id, true);
    if (!checked.result.success) return {checked.result};
    if (!checked.value.tagJournal.isEmpty()) return {changedTags()};
    auto git = repository(id);
    const auto local = git.tagRefs(); const auto endpoint = git.tagEndpoint(); const auto head = git.head();
    if (!local.result.success) return {local.result};
    if (!endpoint.result.success) return {endpoint.result};
    if (!head.result.success) return {head.result};
    PreparedRebuild prepared{id, head.value, endpoint.value, checked.value.generation, local.value, {}, {}};
    auto result = OperationResult::ok({});
    if (!endpoint.value.isEmpty())
    {
        const auto remoteTags = git.fetchTags(); const auto remoteHead = git.remoteHead();
        if (remoteTags.result.success && remoteHead.result.success)
        { prepared.remoteTags = remoteTags.value; prepared.remoteHead = remoteHead.value; }
        else
        {
            // Only previously observed refs are eligible for a later offline deletion.
            const auto known = checked.value.tagRemotes.value(endpoint.value);
            prepared.remoteTags = known.base;
            prepared.remoteHead = known.lastUploadedHead;
            result.warning = prepared.remoteHead.isEmpty()
                ? "当前无法确认云端。本次可重建本地；云端版本覆盖需联网后重新确认。已确认的标记删除会保留待同步记录。"
                : "当前无法连接云端。本次先重建本地，下次上传时仅覆盖上次成功上传的云端版本；云端已有新修改时会停止覆盖。";
        }
    }
    return {result, prepared};
}
