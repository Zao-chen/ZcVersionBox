#include "aicommitmessagehelper.h"

#include <QEventLoop>
#include <QTimer>

#include "AiProvider.h"
#include "aiconfighelper.h"

namespace
{

void setupProvider(AiProvider &provider,
                   const AiConfigHelper::RuntimeConfig &config,
                   const QString &systemPrompt)
{
    provider.setServiceType(config.serviceType);
    // Custom 才覆盖 Base URL。
    if (config.serviceType == AiProvider::Custom)
        provider.setBaseUrl(AiConfigHelper::deriveBaseUrl(config.baseUrl));
    provider.setApiKey(config.apiKey);
    provider.setModel(config.modelName);
    provider.setStreamEnabled(false);
    provider.setSystemPrompt(systemPrompt);
}

} // namespace

namespace AiCommitMessageHelper
{

QString buildPromptFromDiff(const QString &diffText)
{
    return QStringLiteral("请根据下面的 git diff 生成 commit message：\n\n%1").arg(diffText);
}

QString systemPrompt()
{
    return QStringLiteral("你是一个资深的 Git 提交信息助手。根据用户提供的 git diff 生成一条简洁、准确的 commit message，只返回提交信息本身，不要返回解释、列表、代码块或多余标点。");
}

QString buildDiffSummaryPrompt(const QString &diffText)
{
    return QStringLiteral("请分析下面的 git diff，用中文输出版本对比结果。要求：先用一句话概括整体变化，再列出 3 到 6 条关键变更；如果发现潜在风险或需要注意的地方，请单独写一行“注意：...”。\n\n%1").arg(diffText);
}

QString diffSummarySystemPrompt()
{
    return QStringLiteral("你是一个资深代码审阅助手。根据用户提供的 git diff 做清晰、简洁的中文版本对比分析，重点说明改了什么、影响什么、是否有风险。不要输出代码块。");
}

QString generateCommitMessageSync(const QString &diffText, int timeoutMs, QString *errorMessage, const QString &settingsFile)
{
    AiConfigHelper::RuntimeConfig config;
    QString configError;
    if (!AiConfigHelper::loadRuntimeConfig(config, &configError, settingsFile))
    {
        if (errorMessage)
            *errorMessage = configError;
        return {};
    }

    AiProvider provider;
    setupProvider(provider, config, systemPrompt());

    QString generated;
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);

    QObject::connect(&provider, &AiProvider::replyReceived, &loop, [&](const QString &reply)
                     {
                         generated = reply.trimmed();
                         loop.quit(); });

    QObject::connect(&provider, &AiProvider::errorOccurred, &loop, [&](const QString &error)
                     {
                         if (errorMessage)
                             *errorMessage = error;
                         loop.quit(); });

    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, [&]()
                     {
                         if (errorMessage)
                             *errorMessage = QStringLiteral("AI 请求超时");
                         loop.quit(); });

    timeoutTimer.start(timeoutMs);
    provider.chat(buildPromptFromDiff(diffText));
    loop.exec();
    timeoutTimer.stop();

    if (generated.isEmpty() && errorMessage && errorMessage->isEmpty())
        *errorMessage = QStringLiteral("AI 未返回有效提交说明");

    return generated;
}

} // namespace AiCommitMessageHelper
