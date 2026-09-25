#include "aigateway.h"
#include "utils/aicommitmessagehelper.h"
#include <QPointer>
#include <QTimer>
#include <memory>

namespace
{
AiProvider *providerFor(const AiConfigHelper::RuntimeConfig &config, QObject *context)
{
    auto *provider = new AiProvider(context);
    provider->setServiceType(config.serviceType);
    provider->setApiKey(config.apiKey);
    if (config.serviceType == AiProvider::Custom)
        provider->setBaseUrl(config.baseUrl);
    provider->setModel(config.modelName);
    provider->setStreamEnabled(false);
    return provider;
}
} // namespace
void AiGateway::fetchModels(const AiConfigHelper::RuntimeConfig &config, QObject *context, ModelsCallback callback)
{
    auto *provider = providerFor(config, context);
    const auto finished = std::make_shared<bool>(false);
    const auto complete = [guard = QPointer<AiProvider>(provider), finished, callback](const QStringList &models, const QString &error)
    {
        if (!guard || *finished)
            return;
        *finished = true;
        guard->deleteLater();
        callback(models, error);
    };
    connect(provider, &AiProvider::modelsReceived, context, [complete](const QList<AiProvider::ModelInfo> &models)
            {
                QStringList ids;
                for (const auto &model : models)
                    if (!model.id.isEmpty())
                        ids.append(model.id);
                complete(ids, {});
            });
    connect(provider, &AiProvider::errorOccurred, context, [complete](const QString &error)
            {
                complete({}, error);
            });
    QTimer::singleShot(30000, provider, [complete]
                       {
                           complete({}, "模型请求超时，请稍后重试");
                       });
    provider->fetchModels();
}
void AiGateway::summarize(const AiConfigHelper::RuntimeConfig &config, const QString &diff, QObject *context, SummaryCallback callback)
{
    auto *provider = providerFor(config, context);
    provider->setSystemPrompt(AiCommitMessageHelper::diffSummarySystemPrompt());
    const auto finished = std::make_shared<bool>(false);
    const auto complete = [guard = QPointer<AiProvider>(provider), finished, callback](const QString &reply, const QString &error)
    {
        if (!guard || *finished)
            return;
        *finished = true;
        guard->deleteLater();
        callback(reply, error);
    };
    connect(provider, &AiProvider::replyReceived, context, [complete](const QString &reply)
            {
                complete(reply.trimmed(), reply.trimmed().isEmpty() ? QStringLiteral("AI 未返回有效对比分析") : QString());
            });
    connect(provider, &AiProvider::errorOccurred, context, [complete](const QString &error)
            {
                complete({}, error);
            });
    QTimer::singleShot(30000, provider, [complete]
                       {
                           complete({}, "AI 分析超时，请稍后重试");
                       });
    provider->chat(AiCommitMessageHelper::buildDiffSummaryPrompt(diff));
}
