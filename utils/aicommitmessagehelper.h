#ifndef AICOMMITMESSAGEHELPER_H
#define AICOMMITMESSAGEHELPER_H

#include <QString>

namespace AiCommitMessageHelper
{

QString buildPromptFromDiff(const QString &diffText);

QString systemPrompt();

QString buildDiffSummaryPrompt(const QString &diffText);

QString diffSummarySystemPrompt();

} // namespace AiCommitMessageHelper

#endif // AICOMMITMESSAGEHELPER_H
