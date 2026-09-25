#include "settingsservice.h"
#include <QCoreApplication>
#include <QPointer>
#include <QSettings>
#ifdef Q_OS_MACOS
#include "macos/macos_services.h"
#endif

namespace Config = AiConfigHelper;
SettingsService::SettingsService(const AppPaths &paths, AiGateway *gateway, QObject *parent)
    : QObject(parent), m_paths(paths), m_gateway(gateway)
{
    QSettings settings(paths.settingsFile, QSettings::IniFormat);
    Config::migrateLegacySettings(settings);
    Config::syncActiveConfig(settings, provider());
}
QVariant SettingsService::value(const QString &key, const QVariant &fallback) const
{
    QSettings settings(m_paths.settingsFile, QSettings::IniFormat);
    return settings.value(key, fallback);
}
QString SettingsService::themeMode() const
{
    const auto mode = value("Theme", "system").toString();
    return mode == QStringLiteral("light") || mode == QStringLiteral("dark") ? mode : QStringLiteral("system");
}
void SettingsService::setThemeMode(const QString &mode)
{
    const auto normalized = mode == QStringLiteral("light") || mode == QStringLiteral("dark") ? mode : QStringLiteral("system");
    if (themeMode() == normalized)
        return;
    QSettings settings(m_paths.settingsFile, QSettings::IniFormat);
    settings.setValue("Theme", normalized);
    emit changed();
}
QString SettingsService::provider() const { return Config::normalizeProviderName(value("AI/Provider", "OpenAI").toString()); }
AiConfigHelper::RuntimeConfig SettingsService::config(const QString &name) const
{
    QSettings settings(m_paths.settingsFile, QSettings::IniFormat);
    return Config::loadProviderConfig(settings, name);
}
bool SettingsService::runtimeConfig(AiConfigHelper::RuntimeConfig &config, QString &error) const
{
    return Config::loadRuntimeConfig(config, &error, m_paths.settingsFile);
}
void SettingsService::invalidateRequest()
{
    ++m_generation;
    if (m_fetching)
    {
        m_fetching = false;
        emit fetchingChanged(false);
    }
}
void SettingsService::selectProvider(const QString &name)
{
    invalidateRequest();
    QSettings settings(m_paths.settingsFile, QSettings::IniFormat);
    Config::syncActiveConfig(settings, Config::normalizeProviderName(name));
    emit changed();
}
void SettingsService::setAiEnabled(bool enabled)
{
    QSettings settings(m_paths.settingsFile, QSettings::IniFormat);
    settings.setValue("AI/Enabled", enabled);
    settings.setValue("AI/AutoCommitMessage", enabled);
    emit changed();
}
void SettingsService::saveField(const QString &field, const QString &text)
{
    if (field != "ApiKey" && field != "BaseUrl" && field != "Model")
        return;
    const auto name = provider();
    if (field == "BaseUrl" && !Config::isCustomProvider(name))
        return;
    const auto normalized = field == "BaseUrl" ? Config::deriveBaseUrl(text) : text.trimmed();
    QSettings settings(m_paths.settingsFile, QSettings::IniFormat);
    const auto key = Config::providerPrefix(name) + field;
    if (settings.value(key).toString() == normalized)
        return;
    settings.setValue(key, normalized);
    if (field != "Model")
    {
        invalidateRequest();
        Config::clearProviderModels(settings, name);
    }
    Config::syncActiveConfig(settings, name);
    emit changed();
}
void SettingsService::fetchModels()
{
    invalidateRequest();
    const auto name = provider();
    const auto request = config(name);
    if (request.apiKey.isEmpty() || (Config::isCustomProvider(name) && request.baseUrl.isEmpty()))
    {
        emit notification(OperationResult::warn("模型获取失败", "请先填写当前服务商的 API Key 和接口地址"));
        return;
    }
    const auto generation = m_generation;
    m_fetching = true;
    emit fetchingChanged(true);
    QPointer<SettingsService> guard(this);
    m_gateway->fetchModels(request, this, [guard, name, generation](const QStringList &models, const QString &error)
                           {
                               if (!guard || generation != guard->m_generation || name != guard->provider())
                                   return;
                               guard->m_fetching = false;
                               emit guard->fetchingChanged(false);
                               if (!error.isEmpty())
                               {
                                   emit guard->notification(OperationResult::warn("模型获取失败", error, 3500));
                                   return;
                               }
                               QSettings settings(guard->m_paths.settingsFile, QSettings::IniFormat);
                               const auto prefix = Config::providerPrefix(name);
                               const auto current = settings.value(prefix + "Model").toString();
                               settings.setValue(prefix + "ModelList", models);
                               settings.setValue(prefix + "Model", current.isEmpty() && !models.isEmpty() ? models.first() : current);
                               Config::syncActiveConfig(settings, name);
                               emit guard->changed(); });
}
OperationResult SettingsService::setSystemOption(const QString &key, bool enabled)
{
    if (key != "RightClickMenu" && key != "AutoStart")
        return OperationResult::fail("设置失败", "未知设置");
#ifdef Q_OS_WIN
    const auto app = QCoreApplication::applicationFilePath().replace('/', '\\');
    if (key == "AutoStart")
    {
        QSettings run(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)", QSettings::NativeFormat);
        if (enabled)
            run.setValue("ZcVersionBox", '"' + app + '"');
        else
            run.remove("ZcVersionBox");
    }
    else
    {
        const QStringList roots{R"(HKEY_CURRENT_USER\Software\Classes\*\shell)", R"(HKEY_CURRENT_USER\Software\Classes\Directory\shell)", R"(HKEY_CURRENT_USER\Software\Classes\Directory\Background\shell)"};
        for (int i = 0; i < roots.size(); ++i)
        {
            QSettings menu(roots[i], QSettings::NativeFormat);
            if (!enabled)
                menu.remove("ZcVersionOpen");
            else
            {
                menu.setValue("ZcVersionOpen/.", "使用ZcVersionBox自动备份");
                menu.setValue("ZcVersionOpen/command/.", QString("\"%1\" \"%2\"").arg(app, i == 2 ? "%V" : "%1"));
            }
        }
    }
#elif defined(Q_OS_MACOS)
    if (key == "RightClickMenu")
        setMacServicesProviderEnabled(enabled);
    else if (!setMacAutoStartEnabled(enabled))
        return OperationResult::fail("开机自启设置失败", "无法写入用户 LaunchAgent");
#endif
    QSettings settings(m_paths.settingsFile, QSettings::IniFormat);
    settings.setValue(key, enabled);
    emit changed();
#ifdef Q_OS_MACOS
    return OperationResult::info(key == "RightClickMenu" ? "右键菜单快捷入口" : "开机自启",
                                 key == "RightClickMenu" ? (enabled ? "已启用 Finder 服务：添加到 ZcVersionBox" : "已停用 Finder 服务")
                                                         : (enabled ? "已启用 macOS 登录时自动启动" : "已停用 macOS 登录时自动启动"));
#else
    return OperationResult::info("设置已更新", enabled ? "已启用" : "已停用");
#endif
}
