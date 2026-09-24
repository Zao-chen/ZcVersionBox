#ifndef AICONFIGHELPER_H
#define AICONFIGHELPER_H

#include "AiProvider.h"

#include <QSettings>
#include <QString>
#include <QStringList>

namespace AiConfigHelper
{

struct RuntimeConfig
{
    QString providerName;
    AiProvider::ServiceType serviceType{AiProvider::OpenAI};
    QString baseUrl;
    QString apiKey;
    QString modelName;
    QStringList modelList;
};

QString openAIProviderName();
QString deepSeekProviderName();
QString customProviderName();

// 规范化和展示服务商名称。
QString normalizeProviderName(const QString &providerName);
QString providerDisplayName(const QString &providerName);

// 定位服务商在 QSettings 中的配置组。
QString providerPrefix(const QString &providerName);

// 映射到 ZcAILib 类型，内置服务商端点由 ZcAILib 管理。
AiProvider::ServiceType serviceTypeForProvider(const QString &providerName);
bool isCustomProvider(const QString &providerName);

// Custom 服务的 URL 归一化。
QString deriveBaseUrl(const QString &apiUrl);

// 把旧 AI/* 配置迁移到分服务商配置。
void migrateLegacySettings(QSettings &ini);

// 读取某个服务商的保存配置。
RuntimeConfig loadProviderConfig(QSettings &ini, const QString &providerName, bool fallbackLegacy = false);

// 读取实际请求 AI 时需要的配置。
bool loadRuntimeConfig(RuntimeConfig &config, QString *errorMessage = nullptr);

// 判断服务商是否已填好 Key 和模型。
bool isProviderConfigured(QSettings &ini, const QString &providerName);

// 把当前服务商配置写回旧 AI/* 运行时入口。
void syncActiveConfig(QSettings &ini, const QString &providerName);

// Key 或 URL 变化后清空模型缓存。
void clearProviderModels(QSettings &ini, const QString &providerName);

} // namespace AiConfigHelper

#endif // AICONFIGHELPER_H
