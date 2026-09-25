#include "aicommitmessagehelper.h"

namespace AiCommitMessageHelper
{

QString buildPromptFromDiff(const QString &diffText)
{
    return QStringLiteral("请根据下面的 git diff 生成 commit message：\n\n%1").arg(diffText);
}

QString systemPrompt()
{
    return QStringLiteral("你是一个资深的 Git 提交信息助手。根据用户提供的 git diff 生成一条简洁、准确的 commit message，只返回提交信息本身，不要返回解释、列表、代码块或多余标点。");
}

QString buildDiffSummaryPrompt(const QString &diffText)
{
    return QStringLiteral("请分析下面的 git diff，用中文输出版本对比结果。要求：先用一句话概括整体变化，再列出 3 到 6 条关键变更；如果发现潜在风险或需要注意的地方，请单独写一行“注意：...”。\n\n%1").arg(diffText);
}

QString diffSummarySystemPrompt()
{
    return QStringLiteral("你是一个资深代码审阅助手。根据用户提供的 git diff 做清晰、简洁的中文版本对比分析，重点说明改了什么、影响什么、是否有风险。不要输出代码块。");
}

} // namespace AiCommitMessageHelper
