#pragma once
#include "utils/aiconfighelper.h"
#include <QObject>
#include <functional>

// Injectable transport boundary; pages never own AiProvider or network replies.
class AiGateway : public QObject
{
    Q_OBJECT
  public:
    using QObject::QObject;
    using ModelsCallback = std::function<void(const QStringList &, const QString &)>;
    using SummaryCallback = std::function<void(const QString &, const QString &)>;
    virtual void fetchModels(const AiConfigHelper::RuntimeConfig &config, QObject *context, ModelsCallback callback);
    virtual void summarize(const AiConfigHelper::RuntimeConfig &config, const QString &diff, QObject *context, SummaryCallback callback);
    virtual void generateCommitMessage(const AiConfigHelper::RuntimeConfig &config, const QString &diff, QObject *context, SummaryCallback callback);
};
