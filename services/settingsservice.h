#pragma once
#include "aigateway.h"
#include "apppaths.h"
#include "operationresult.h"
#include <QMap>
#include <QVariant>

class SettingsService : public QObject
{
    Q_OBJECT
  public:
    SettingsService(const AppPaths &paths, AiGateway *gateway, QObject *parent = nullptr);
    QString provider() const;
    void selectProvider(const QString &name);
    AiConfigHelper::RuntimeConfig config(const QString &provider) const;
    QVariant value(const QString &key, const QVariant &fallback = {}) const;
    void setAiEnabled(bool enabled);
    void saveField(const QString &field, const QString &text);
    void fetchModels();
    bool runtimeConfig(AiConfigHelper::RuntimeConfig &config, QString &error) const;
    OperationResult setSystemOption(const QString &key, bool enabled);
  signals:
    void changed();
    void fetchingChanged(bool fetching);
    void notification(const OperationResult &result);

  private:
    AppPaths m_paths;
    AiGateway *m_gateway;
    quint64 m_generation{0};
    bool m_fetching{false};
    void invalidateRequest();
};
