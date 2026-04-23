#ifndef AICOMMITMESSAGEHELPER_H
#define AICOMMITMESSAGEHELPER_H

#include <QString>

#include <functional>

class QObject;

namespace AiCommitMessageHelper
{

bool isAutoCommitEnabled();

bool loadAiConfig(QString &baseUrl, QString &apiKey, QString &modelName, QString *errorMessage = nullptr);

QString buildPromptFromDiff(const QString &diffText);

QString systemPrompt();

void generateCommitMessageAsync(const QString &diffText,
                                QObject *context,
                                const std::function<void(const QString &)> &onSuccess,
                                const std::function<void(const QString &)> &onError);

QString generateCommitMessageSync(const QString &diffText,
                                  int timeoutMs = 15000,
                                  QString *errorMessage = nullptr);

} // namespace AiCommitMessageHelper

#endif // AICOMMITMESSAGEHELPER_H
