#include "aicommitmessagehelper.h"

#include "../GlobalConstants.h"

#include <QEventLoop>
#include <QSettings>
#include <QTimer>

#include "aiprovider.h"

namespace
{

QString deriveModelsUrl(const QString &apiUrl)
{
    QString url = apiUrl.trimmed();
    if (url.isEmpty())
        return {};

    if (url.endsWith("/chat/completions"))
    {
        url.chop(QString("/chat/completions").size());
        return url + "/models";
    }

    if (url.endsWith("/v1"))
        return url + "/models";

    if (url.endsWith('/'))
        url.chop(1);

    return url + "/models";
}

void setupProvider(AiProvider &provider,
                   const QString &baseUrl,
                   const QString &apiKey,
                   const QString &modelName)
{
    provider.setServiceType(AiProvider::Custom);
    provider.setApiUrl(baseUrl);
    provider.setModelsApiUrl(deriveModelsUrl(baseUrl));
    provider.setApiKey(apiKey);
    provider.setModel(modelName);
    provider.setStreamEnabled(false);
    provider.setSystemPrompt(AiCommitMessageHelper::systemPrompt());
}

} // namespace

namespace AiCommitMessageHelper
{

bool isAutoCommitEnabled()
{
    QSettings ini(Settingpath, QSettings::IniFormat);
    return ini.value("AI/AutoCommitMessage", false).toBool();
}

bool loadAiConfig(QString &baseUrl, QString &apiKey, QString &modelName, QString *errorMessage)
{
    QSettings ini(Settingpath, QSettings::IniFormat);
    baseUrl = ini.value("AI/BaseUrl", QString()).toString().trimmed();
    apiKey = ini.value("AI/ApiKey", QString()).toString().trimmed();
    modelName = ini.value("AI/Model", QString()).toString().trimmed();

    if (!baseUrl.isEmpty() && !apiKey.isEmpty() && !modelName.isEmpty())
        return true;

    if (errorMessage)
        *errorMessage = QStringLiteral("请先在设置中填写 Base URL、API Key 和模型");
    return false;
}

QString buildPromptFromDiff(const QString &diffText)
{
    return QStringLiteral("请根据下面的 git diff 生成 commit message：\n\n%1").arg(diffText);
}

QString systemPrompt()
{
    return QStringLiteral("你是一个资深的 Git 提交信息助手。根据用户提供的 git diff 生成一条简洁、准确的 commit message，只返回提交信息本身，不要返回解释、列表、代码块或多余标点。");
}

void generateCommitMessageAsync(const QString &diffText,
                                QObject *context,
                                const std::function<void(const QString &)> &onSuccess,
                                const std::function<void(const QString &)> &onError)
{
    QString baseUrl;
    QString apiKey;
    QString modelName;
    QString errorMessage;
    if (!loadAiConfig(baseUrl, apiKey, modelName, &errorMessage))
    {
        if (onError)
            onError(errorMessage);
        return;
    }

    auto *provider = new AiProvider(context);
    setupProvider(*provider, baseUrl, apiKey, modelName);

    QObject::connect(provider, &AiProvider::replyReceived, context, [provider, onSuccess, onError](const QString &reply)
                     {
                         const QString generated = reply.trimmed();
                         if (generated.isEmpty())
                         {
                             if (onError)
                                 onError(QStringLiteral("AI 未返回有效提交说明"));
                         }
                         else if (onSuccess)
                         {
                             onSuccess(generated);
                         }
                         provider->deleteLater(); });

    QObject::connect(provider, &AiProvider::errorOccurred, context, [provider, onError](const QString &error)
                     {
                         if (onError)
                             onError(error);
                         provider->deleteLater(); });

    provider->chat(buildPromptFromDiff(diffText));
}

QString generateCommitMessageSync(const QString &diffText, int timeoutMs, QString *errorMessage)
{
    QString baseUrl;
    QString apiKey;
    QString modelName;
    QString configError;
    if (!loadAiConfig(baseUrl, apiKey, modelName, &configError))
    {
        if (errorMessage)
            *errorMessage = configError;
        return {};
    }

    AiProvider provider;
    setupProvider(provider, baseUrl, apiKey, modelName);

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
