#pragma once
#include "aigateway.h"
#include "apppaths.h"
#include "operationresult.h"
#include "linuxintegration.h"
#include <QMap>
#include <QVariant>

class SettingsService : public QObject
{
    Q_OBJECT
  public:
    SettingsService(const AppPaths &paths, AiGateway *gateway, QObject *parent = nullptr,
                    LinuxIntegrationPaths systemPaths = LinuxIntegrationPaths::defaults());
    QString provider() const;
    void selectProvider(const QString &name);
    AiConfigHelper::RuntimeConfig config(const QString &provider) const;
    QVariant value(const QString &key, const QVariant &fallback = {}) const;
    void setAiEnabled(bool enabled);
    void saveField(const QString &field, const QString &text);
    void fetchModels();
    bool isFetching() const { return m_fetching; }
    bool runtimeConfig(AiConfigHelper::RuntimeConfig &config, QString &error) const;
    OperationResult setSystemOption(const QString &key, bool enabled);
    QString themeMode() const;
    void setThemeMode(const QString &mode);
  signals:
    void changed();
    void fetchingChanged(bool fetching);
    void notification(const OperationResult &result);

  private:
    AppPaths m_paths;
    AiGateway *m_gateway;
    LinuxIntegration m_linuxIntegration;
    quint64 m_generation{0};
    bool m_fetching{false};
    void invalidateRequest();
};
