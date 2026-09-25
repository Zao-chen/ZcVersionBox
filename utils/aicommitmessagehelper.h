#ifndef AICOMMITMESSAGEHELPER_H
#define AICOMMITMESSAGEHELPER_H

#include <QString>

namespace AiCommitMessageHelper
{

QString buildPromptFromDiff(const QString &diffText);

QString systemPrompt();

QString buildDiffSummaryPrompt(const QString &diffText);

QString diffSummarySystemPrompt();

QString generateCommitMessageSync(const QString &diffText,
                                  int timeoutMs = 15000,
                                  QString *errorMessage = nullptr,
                                  const QString &settingsFile = {});

} // namespace AiCommitMessageHelper

#endif // AICOMMITMESSAGEHELPER_H
