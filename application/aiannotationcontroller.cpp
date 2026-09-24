#include "aiannotationcontroller.h"
#include "GlobalConstants.h"
#include "utils/aiconfighelper.h"
#include <QDir>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

AiAnnotationController::AiAnnotationController(Backup::BackupService *service, QObject *parent) : QObject(parent), m_service(service)
{
    connect(service, &Backup::BackupService::snapshotCommitted, this, [this](const QString &id, const QString &revision)
            {
                if (enabled())
                    enqueue(id, revision);
            });
    connect(service, &Backup::BackupService::aiAnnotationRequested, this, &AiAnnotationController::enqueue);
    connect(service, &Backup::BackupService::trackingChanged, this, &AiAnnotationController::resume);
    connect(service, &Backup::BackupService::trackingResetRequested, this, [this](const QString &id)
            {
                for (const auto &key : m_jobs.keys())
                    if (m_jobs[key].id == id)
                        complete(key, "备份对象正在删除或重建，备注任务已取消");
            });
    connect(service, &Backup::BackupService::taskFinished, this, [this](const Backup::TaskResult &result)
            {
                if (result.data["removed"].toBool())
                {
                    for (const auto &key : m_jobs.keys())
                        if (m_jobs[key].id == result.trackingId)
                            complete(key, "备份对象已删除");
                }
                if (!m_requests.contains(result.requestId))
                    return;
                const auto key = m_requests.take(result.requestId);
                if (!m_jobs.contains(key))
                    return;
                if (!result.success)
                {
                    complete(key, result.message, result.retryable);
                    return;
                }
                if (result.operation == "diff")
                    generate(key, result.data["text"].toString());
                else
                    complete(key);
            });
}
AiAnnotationController::~AiAnnotationController()
{
    for (const auto &job : m_jobs)
        if (job.context)
            delete job.context;
}
bool AiAnnotationController::enabled() const { return QSettings(settingsPath(), QSettings::IniFormat).value("AI/Enabled", false).toBool(); }
void AiAnnotationController::persist(const Job &job, const QString &state, const QString &error)
{
    const auto object = QDir(m_service->root()).filePath("objects/" + job.id);
    if (!QFileInfo::exists(object + "/config.json"))
        return;
    try
    {
        Backup::writeJson(object + "/ai/" + job.revision + ".json", {{"version", 1}, {"revision", job.revision}, {"state", state}, {"error", error}});
    }
    catch (const Backup::Error &e)
    {
        emit m_service->stateChanged(job.id, "ai", "无法保存 AI 任务：" + e.message);
    }
}
void AiAnnotationController::enqueue(const QString &id, const QString &revision)
{
    if (m_service->shuttingDown() || !QRegularExpression("^[0-9a-f]{40}$").match(revision).hasMatch())
        return;
    const auto key = id + '/' + revision;
    if (m_jobs.contains(key))
        return;
    bool exists = false;
    for (const auto &t : m_service->trackings())
        if (t.id == id)
            exists = true;
    if (!exists)
        return;
    Job job{id, revision};
    m_jobs.insert(key, job);
    persist(job, "queued");
    m_queue.enqueue(key);
    dispatch();
}
void AiAnnotationController::resume()
{
    if (!enabled() || m_service->shuttingDown())
        return;
    for (const auto &tracking : m_service->trackings())
    {
        QDir dir(QDir(m_service->root()).filePath("objects/" + tracking.id + "/ai"));
        for (const auto &name : dir.entryList({"*.json"}, QDir::Files))
        {
            try
            {
                const auto job = Backup::readJson(dir.filePath(name));
                if (job["version"].toInt() != 1)
                    throw Backup::Error(Backup::ErrorCode::InvalidFormat, "不支持的 AI 任务格式");
                if (job["state"] == "queued" || job["state"] == "running")
                    enqueue(tracking.id, job["revision"].toString());
            }
            catch (const Backup::Error &e)
            {
                emit m_service->stateChanged(tracking.id, "ai", e.message);
            }
        }
    }
}
void AiAnnotationController::dispatch()
{
    if (m_service->shuttingDown())
        return;
    while (m_active < 2 && !m_queue.isEmpty())
    {
        const auto key = m_queue.dequeue();
        if (!m_jobs.contains(key))
            continue;
        auto &job = m_jobs[key];
        job.active = true;
        ++m_active;
        persist(job, "running");
        emit m_service->stateChanged(job.id, "ai", "正在准备 AI 备注");
        Backup::Request r;
        r.requestId = Backup::newId();
        r.trackingId = job.id;
        r.revision = job.revision;
        r.operation = "diff";
        m_requests.insert(r.requestId, key);
        if (m_service->submit(r).isEmpty())
            complete(key, "备份对象不可用");
    }
}
void AiAnnotationController::generate(const QString &key, const QString &diff)
{
    if (!m_jobs.contains(key) || m_service->shuttingDown())
        return;
    QSettings settings(settingsPath(), QSettings::IniFormat);
    const auto providerName = settings.value("AI/Provider").toString();
    if (!QStringList{AiConfigHelper::openAIProviderName(), AiConfigHelper::deepSeekProviderName(), AiConfigHelper::customProviderName()}.contains(providerName))
    {
        complete(key, "AI 服务商未配置或不受支持");
        return;
    }
    const auto config = AiConfigHelper::loadProviderConfig(settings, providerName);
    if (providerName.isEmpty() || config.apiKey.isEmpty() || config.modelName.isEmpty() || (AiConfigHelper::isCustomProvider(providerName) && config.baseUrl.isEmpty()))
    {
        complete(key, "请在设置中配置 AI 服务商、密钥和模型");
        return;
    }
    auto context = new QObject(this);
    m_jobs[key].context = context;
    auto provider = new AiProvider(context);
    provider->setServiceType(config.serviceType);
    provider->setApiKey(config.apiKey);
    provider->setModel(config.modelName);
    provider->setStreamEnabled(false);
    if (AiConfigHelper::isCustomProvider(providerName))
        provider->setBaseUrl(AiConfigHelper::deriveBaseUrl(config.baseUrl));
    provider->setSystemPrompt("根据文件版本变化，用简洁中文描述这次修改。只返回说明，不添加前言。");
    auto timeout = new QTimer(context);
    timeout->setSingleShot(true);
    timeout->setInterval(15000);
    connect(timeout, &QTimer::timeout, context, [this, key, context]
            {
                if (!m_jobs.contains(key) || m_jobs[key].context != context)
                    return;
                complete(key, "AI 请求超时", true);
            });
    connect(provider, &AiProvider::errorOccurred, context, [this, key, context](const QString &error)
            {
                if (!m_jobs.contains(key) || m_jobs[key].context != context)
                    return;
                const auto retryable = error.contains("timeout", Qt::CaseInsensitive) || error.contains("network", Qt::CaseInsensitive) || error.contains("connection", Qt::CaseInsensitive);
                complete(key, error, retryable);
            });
    connect(provider, &AiProvider::replyReceived, context, [this, key, timeout, context, provider](const QString &reply)
            {
                timeout->stop();
                if (!m_jobs.contains(key) || m_jobs[key].context != context)
                    return;
                disconnect(provider, nullptr, context, nullptr);
                if (reply.trimmed().isEmpty())
                {
                    complete(key, "AI 返回空说明");
                    return;
                }
                const auto job = m_jobs[key];
                Backup::Request r;
                r.requestId = Backup::newId();
                r.operation = "annotateAi";
                r.trackingId = job.id;
                r.revision = job.revision;
                r.text = reply.trimmed();
                m_requests.insert(r.requestId, key);
                if (m_service->submit(r).isEmpty())
                    complete(key, "备份对象不可用");
            });
    timeout->start();
    emit m_service->stateChanged(m_jobs[key].id, "ai", "正在生成备注");
    provider->chat("请说明以下快照变化：\n" + diff.left(60000));
}
void AiAnnotationController::complete(const QString &key, const QString &error, bool retryable)
{
    if (!m_jobs.contains(key))
        return;
    auto job = m_jobs.take(key);
    for (auto it = m_requests.begin(); it != m_requests.end();)
        if (it.value() == key)
            it = m_requests.erase(it);
        else
            ++it;
    if (job.context)
        job.context->deleteLater();
    if (job.active)
        --m_active;
    persist(job, error.isEmpty() ? "completed" : "failed", error);
    emit m_service->stateChanged(job.id, "ai", error.isEmpty() ? "备注已保存" : "备注失败：" + error);
    if (retryable && !m_service->shuttingDown())
    {
        const int delays[] = {5000, 15000, 60000};
        const auto delay = delays[qMin(job.attempts++, 2)];
        job.context.clear();
        job.active = false;
        m_jobs.insert(key, job);
        persist(job, "queued", error);
        QTimer::singleShot(delay, this, [this, key]
                           {
                               if (m_jobs.contains(key))
                               {
                                   m_queue.enqueue(key);
                                   dispatch();
                               }
                           });
    }
    dispatch();
}
