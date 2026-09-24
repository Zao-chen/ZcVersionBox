#include "aiconfighelper.h"

#include "../GlobalConstants.h"

namespace
{

const QString kProviderOpenAI = QStringLiteral("OpenAI");
const QString kProviderDeepSeek = QStringLiteral("DeepSeek");
const QString kProviderCustom = QStringLiteral("Custom");

QString providerFromLegacyBaseUrl(const QString &baseUrl)
{
    const QString normalized = baseUrl.trimmed().toLower();
    if (normalized.contains(QStringLiteral("api.deepseek.com")))
        return kProviderDeepSeek;
    if (normalized.contains(QStringLiteral("api.openai.com")))
        return kProviderOpenAI;
    return kProviderCustom;
}

} // namespace

namespace AiConfigHelper
{

QString openAIProviderName()
{
    return kProviderOpenAI;
}

QString deepSeekProviderName()
{
    return kProviderDeepSeek;
}

QString customProviderName()
{
    return kProviderCustom;
}

// 统一服务商名称。
QString normalizeProviderName(const QString &providerName)
{
    if (providerName.compare(kProviderDeepSeek, Qt::CaseInsensitive) == 0)
        return kProviderDeepSeek;
    if (providerName.compare(kProviderCustom, Qt::CaseInsensitive) == 0)
        return kProviderCustom;
    return kProviderOpenAI;
}

// 服务商显示名称。
QString providerDisplayName(const QString &providerName)
{
    const QString normalized = normalizeProviderName(providerName);
    if (normalized == kProviderDeepSeek)
        return QStringLiteral("DeepSeek");
    if (normalized == kProviderCustom)
        return QStringLiteral("Custom");
    return QStringLiteral("OpenAI");
}

// 服务商配置前缀。
QString providerPrefix(const QString &providerName)
{
    return QStringLiteral("AI/Providers/%1/").arg(normalizeProviderName(providerName));
}

// 映射到 ZcAILib 服务类型。
AiProvider::ServiceType serviceTypeForProvider(const QString &providerName)
{
    const QString normalized = normalizeProviderName(providerName);
    if (normalized == kProviderDeepSeek)
        return AiProvider::DeepSeek;
    if (normalized == kProviderCustom)
        return AiProvider::Custom;
    return AiProvider::OpenAI;
}

bool isCustomProvider(const QString &providerName)
{
    return normalizeProviderName(providerName) == kProviderCustom;
}

// 统一为 /v1 形式。
QString deriveBaseUrl(const QString &apiUrl)
{
    QString url = apiUrl.trimmed();
    if (url.isEmpty())
        return {};

    if (url.endsWith("/chat/completions"))
    {
        url.chop(QString("/chat/completions").size());
        return url;
    }

    if (url.endsWith("/v1"))
        return url;

    if (url.endsWith('/'))
        url.chop(1);

    return url;
}

void migrateLegacySettings(QSettings &ini)
{
    if (ini.contains("AI/Provider"))
        return;

    const QString legacyBaseUrl = ini.value("AI/BaseUrl").toString().trimmed();
    const QString legacyApiKey = ini.value("AI/ApiKey").toString().trimmed();
    const QString legacyModel = ini.value("AI/Model").toString().trimmed();
    const QString providerName = (!legacyBaseUrl.isEmpty() || !legacyApiKey.isEmpty() || !legacyModel.isEmpty())
                                     ? providerFromLegacyBaseUrl(legacyBaseUrl)
                                     : kProviderOpenAI;
    const QString prefix = providerPrefix(providerName);

    ini.setValue("AI/Provider", providerName);
    if (!legacyApiKey.isEmpty())
        ini.setValue(prefix + "ApiKey", legacyApiKey);
    if (!legacyModel.isEmpty())
        ini.setValue(prefix + "Model", legacyModel);
    if (providerName == kProviderCustom && !legacyBaseUrl.isEmpty())
        ini.setValue(prefix + "BaseUrl", deriveBaseUrl(legacyBaseUrl));
}

RuntimeConfig loadProviderConfig(QSettings &ini, const QString &providerName, bool fallbackLegacy)
{
    RuntimeConfig config;
    config.providerName = normalizeProviderName(providerName);
    config.serviceType = serviceTypeForProvider(config.providerName);

    const QString prefix = providerPrefix(config.providerName);
    const bool custom = isCustomProvider(config.providerName);
    config.baseUrl = custom ? ini.value(prefix + "BaseUrl").toString().trimmed() : QString();
    config.apiKey = ini.value(prefix + "ApiKey").toString().trimmed();
    config.modelName = ini.value(prefix + "Model").toString().trimmed();
    config.modelList = ini.value(prefix + "ModelList").toStringList();

    if (fallbackLegacy)
    {
        if (custom && config.baseUrl.isEmpty())
            config.baseUrl = ini.value("AI/BaseUrl").toString().trimmed();
        if (config.apiKey.isEmpty())
            config.apiKey = ini.value("AI/ApiKey").toString().trimmed();
        if (config.modelName.isEmpty())
            config.modelName = ini.value("AI/Model").toString().trimmed();
    }

    return config;
}

bool loadRuntimeConfig(RuntimeConfig &config, QString *errorMessage)
{
    QSettings ini(Settingpath, QSettings::IniFormat);
    QString providerName = ini.value("AI/Provider").toString().trimmed();
    if (providerName.isEmpty())
        providerName = providerFromLegacyBaseUrl(ini.value("AI/BaseUrl").toString());

    config = loadProviderConfig(ini, providerName, true);
    const bool missingBaseUrl = isCustomProvider(config.providerName) && config.baseUrl.isEmpty();
    if (!missingBaseUrl && !config.apiKey.isEmpty() && !config.modelName.isEmpty())
        return true;

    if (errorMessage)
        *errorMessage = QStringLiteral("请先在设置中选择 AI 服务商，并填写 API Key 和模型");
    return false;
}

bool isProviderConfigured(QSettings &ini, const QString &providerName)
{
    const RuntimeConfig config = loadProviderConfig(ini, providerName);
    const bool hasBaseUrl = !isCustomProvider(config.providerName) || !config.baseUrl.isEmpty();
    return hasBaseUrl && !config.apiKey.isEmpty() && !config.modelName.isEmpty();
}

void syncActiveConfig(QSettings &ini, const QString &providerName)
{
    const RuntimeConfig config = loadProviderConfig(ini, providerName);
    ini.setValue("AI/Provider", config.providerName);
    ini.setValue("AI/BaseUrl", config.baseUrl);
    ini.setValue("AI/ApiKey", config.apiKey);
    ini.setValue("AI/Model", config.modelName);
}

void clearProviderModels(QSettings &ini, const QString &providerName)
{
    const QString prefix = providerPrefix(providerName);
    ini.setValue(prefix + "ModelList", QStringList());
    ini.setValue(prefix + "Model", QString());
}

} // namespace AiConfigHelper
