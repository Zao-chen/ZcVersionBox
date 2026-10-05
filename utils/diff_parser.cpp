#include "diff_parser.h"
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>

namespace DiffParser
{

namespace
{

struct Token
{
    QString text;
    int start;
    int length;
};

// 分词工具：将文本切分为单词、标点、空白或单个中文字符
QVector<Token> tokenize(const QString &text)
{
    QVector<Token> tokens;
    int i = 0;
    const int len = text.length();

    while (i < len)
    {
        QChar c = text.at(i);
        int start = i;

        if (c.isSpace())
        {
            while (i < len && text.at(i).isSpace())
                ++i;
        }
        else if (c.isLetterOrNumber() && c.unicode() < 128)
        {
            while (i < len && text.at(i).isLetterOrNumber() && text.at(i).unicode() < 128)
                ++i;
        }
        else
        {
            // 非 ASCII 字符（如中文）或标点符号单独作为一个 Token
            ++i;
        }

        tokens.append({text.mid(start, i - start), start, i - start});
    }

    return tokens;
}

// 将文本格式化为带行内高亮的 HTML 片段
QString spansToHtml(const QString &text, const QVector<TextSpan> &spans, const QString &wordBgHex)
{
    if (spans.isEmpty())
        return text.toHtmlEscaped();

    QString html;
    for (const auto &span : spans)
    {
        QString sub = text.mid(span.start, span.length).toHtmlEscaped();
        if (span.highlighted && !wordBgHex.isEmpty())
        {
            html += QString("<span style=\"background-color:%1; border-radius:2px; font-weight:600;\">%2</span>")
                        .arg(wordBgHex, sub);
        }
        else
        {
            html += sub;
        }
    }
    return html;
}

} // namespace

// 基于 LCS 的轻量级词级差异计算
void computeWordDiff(const QString &oldText, const QString &newText,
                     QVector<TextSpan> &oldSpans, QVector<TextSpan> &newSpans)
{
    const auto oldTokens = tokenize(oldText);
    const auto newTokens = tokenize(newText);

    const int n = oldTokens.size();
    const int m = newTokens.size();

    if (n == 0 && m == 0)
        return;

    if (n == 0)
    {
        newSpans.append({0, (int)newText.length(), true});
        return;
    }
    if (m == 0)
    {
        oldSpans.append({0, (int)oldText.length(), true});
        return;
    }

    // 限制过长行的 LCS 计算开销（例如大于 150 个 token 则降级）
    if (n > 150 || m > 150)
    {
        oldSpans.append({0, (int)oldText.length(), true});
        newSpans.append({0, (int)newText.length(), true});
        return;
    }

    QVector<QVector<int>> dp(n + 1, QVector<int>(m + 1, 0));
    for (int i = 1; i <= n; ++i)
    {
        for (int j = 1; j <= m; ++j)
        {
            if (oldTokens[i - 1].text == newTokens[j - 1].text)
                dp[i][j] = dp[i - 1][j - 1] + 1;
            else
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
        }
    }

    QVector<bool> oldMatched(n, false);
    QVector<bool> newMatched(m, false);

    int i = n, j = m;
    while (i > 0 && j > 0)
    {
        if (oldTokens[i - 1].text == newTokens[j - 1].text)
        {
            oldMatched[i - 1] = true;
            newMatched[j - 1] = true;
            --i;
            --j;
        }
        else if (dp[i - 1][j] >= dp[i][j - 1])
        {
            --i;
        }
        else
        {
            --j;
        }
    }

    auto makeSpans = [](const QVector<Token> &tokens, const QVector<bool> &matched) {
        QVector<TextSpan> spans;
        int k = 0;
        const int count = tokens.size();
        while (k < count)
        {
            bool isHigh = !matched[k];
            int start = tokens[k].start;
            int totalLen = 0;
            while (k < count && (!matched[k]) == isHigh)
            {
                totalLen += tokens[k].length;
                ++k;
            }
            if (totalLen > 0)
                spans.append({start, totalLen, isHigh});
        }
        return spans;
    };

    oldSpans = makeSpans(oldTokens, oldMatched);
    newSpans = makeSpans(newTokens, newMatched);
}

ParsedDiff parse(const QString &rawDiff)
{
    ParsedDiff result;
    result.rawPatch = rawDiff;

    if (rawDiff.trimmed().isEmpty())
        return result;

    if (rawDiff.contains("Binary files") && rawDiff.contains("differ"))
    {
        result.isBinary = true;
        result.binaryNotice = QStringLiteral("这是一个二进制文件，无法直接对比纯文本内容。");
        return result;
    }
    if (rawDiff.startsWith("GIT binary patch"))
    {
        result.isBinary = true;
        result.binaryNotice = QStringLiteral("这是一个二进制文件变更（Git Binary Patch）。");
        return result;
    }

    QStringList lines = rawDiff.split('\n');
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    static const QRegularExpression hunkHeaderRegex(R"(^@@ -([0-9]+)(?:,([0-9]+))? \+([0-9]+)(?:,([0-9]+))? @@(.*)$)");

    int oldLineNum = 0;
    int newLineNum = 0;
    bool inHunk = false;

    // 暂存一个 hunk 中的行，用于后续配对与 intra-line 差异对比
    struct RawLine
    {
        QChar prefix;
        QString text;
        int oldNum;
        int newNum;
    };

    QVector<RawLine> hunkLines;

    auto flushHunk = [&]() {
        if (hunkLines.isEmpty())
            return;

        int idx = 0;
        const int total = hunkLines.size();

        while (idx < total)
        {
            if (hunkLines[idx].prefix == ' ')
            {
                // 上下文行
                const auto &r = hunkLines[idx];
                SideBySideRow sbs;
                sbs.oldLineNumber = r.oldNum;
                sbs.oldText = r.text;
                sbs.oldType = LineType::Context;
                sbs.newLineNumber = r.newNum;
                sbs.newText = r.text;
                sbs.newType = LineType::Context;
                result.sideBySideRows.append(sbs);

                UnifiedRow uni;
                uni.oldLineNumber = r.oldNum;
                uni.newLineNumber = r.newNum;
                uni.text = r.text;
                uni.type = LineType::Context;
                result.unifiedRows.append(uni);

                ++idx;
            }
            else
            {
                // 收集连续的 deleted 行与 added 行
                QVector<RawLine> delGroup;
                QVector<RawLine> addGroup;

                while (idx < total && hunkLines[idx].prefix == '-')
                {
                    delGroup.append(hunkLines[idx]);
                    ++idx;
                }
                while (idx < total && hunkLines[idx].prefix == '+')
                {
                    addGroup.append(hunkLines[idx]);
                    ++idx;
                }

                // 针对单行替换或等长行，计算 intra-line 词级高亮
                int pairCount = std::min(delGroup.size(), addGroup.size());
                QVector<QVector<TextSpan>> delSpans(delGroup.size());
                QVector<QVector<TextSpan>> addSpans(addGroup.size());

                for (int p = 0; p < pairCount; ++p)
                {
                    computeWordDiff(delGroup[p].text, addGroup[p].text, delSpans[p], addSpans[p]);
                }

                // 添加到 Unified Rows
                for (int d = 0; d < delGroup.size(); ++d)
                {
                    UnifiedRow uni;
                    uni.oldLineNumber = delGroup[d].oldNum;
                    uni.newLineNumber = -1;
                    uni.text = delGroup[d].text;
                    uni.spans = delSpans[d];
                    uni.type = LineType::Deleted;
                    result.unifiedRows.append(uni);
                }
                for (int a = 0; a < addGroup.size(); ++a)
                {
                    UnifiedRow uni;
                    uni.oldLineNumber = -1;
                    uni.newLineNumber = addGroup[a].newNum;
                    uni.text = addGroup[a].text;
                    uni.spans = addSpans[a];
                    uni.type = LineType::Added;
                    result.unifiedRows.append(uni);
                }

                // 添加到 SideBySide Rows (并排对齐)
                int maxRows = std::max(delGroup.size(), addGroup.size());
                for (int r = 0; r < maxRows; ++r)
                {
                    SideBySideRow sbs;
                    if (r < delGroup.size())
                    {
                        sbs.oldLineNumber = delGroup[r].oldNum;
                        sbs.oldText = delGroup[r].text;
                        sbs.oldSpans = delSpans[r];
                        sbs.oldType = LineType::Deleted;
                    }
                    else
                    {
                        sbs.oldType = LineType::Empty;
                    }

                    if (r < addGroup.size())
                    {
                        sbs.newLineNumber = addGroup[r].newNum;
                        sbs.newText = addGroup[r].text;
                        sbs.newSpans = addSpans[r];
                        sbs.newType = LineType::Added;
                    }
                    else
                    {
                        sbs.newType = LineType::Empty;
                    }

                    result.sideBySideRows.append(sbs);
                }
            }
        }

        hunkLines.clear();
    };

    bool hasPreviousHunk = false;
    for (const QString &line : lines)
    {
        auto match = hunkHeaderRegex.match(line);
        if (match.hasMatch())
        {
            flushHunk();
            inHunk = true;
            oldLineNum = match.captured(1).toInt();
            newLineNum = match.captured(3).toInt();

            QString section = match.captured(5).trimmed();

            // 首个 Hunk 无需任何多余横栏；后续跨段 Hunk 仅插入弱化的极简断点
            if (hasPreviousHunk)
            {
                SideBySideRow sbsHeader;
                sbsHeader.isHeader = true;
                sbsHeader.headerText = section.isEmpty() ? QStringLiteral("···") : QStringLiteral("··· (%1) ···").arg(section);
                result.sideBySideRows.append(sbsHeader);

                UnifiedRow uniHeader;
                uniHeader.isHeader = true;
                uniHeader.headerText = sbsHeader.headerText;
                result.unifiedRows.append(uniHeader);
            }
            hasPreviousHunk = true;
            continue;
        }

        if (!inHunk)
            continue;

        if (line.isEmpty())
        {
            hunkLines.append({' ', QString(), oldLineNum++, newLineNum++});
            continue;
        }

        const QChar prefix = line.at(0);
        const QString text = line.mid(1);

        if (prefix == ' ')
        {
            hunkLines.append({' ', text, oldLineNum++, newLineNum++});
        }
        else if (prefix == '-')
        {
            result.deletedCount++;
            hunkLines.append({'-', text, oldLineNum++, -1});
        }
        else if (prefix == '+')
        {
            result.addedCount++;
            hunkLines.append({'+', text, -1, newLineNum++});
        }
        else if (prefix == '\\')
        {
            // 忽略 "\ No newline at end of file"
            continue;
        }
    }

    flushHunk();
    return result;
}

ParsedDiff diffHunk(const QByteArray &beforeBytes, const QByteArray &localBytes,
                    const QByteArray &remoteBytes, const QByteArray &afterBytes,
                    int startLine)
{
    ParsedDiff result;
    auto toLines = [](const QByteArray &bytes) {
        if (bytes.isEmpty())
            return QStringList();
        QString s = QString::fromUtf8(bytes).replace("\r\n", "\n").replace('\r', '\n');
        QStringList lines = s.split('\n');
        if (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        return lines;
    };

    const auto beforeLines = toLines(beforeBytes);
    const auto localLines = toLines(localBytes);
    const auto remoteLines = toLines(remoteBytes);
    const auto afterLines = toLines(afterBytes);

    int oldNum = startLine;
    int newNum = startLine;

    // 1. Context before hunk
    for (const auto &line : beforeLines)
    {
        SideBySideRow sbs;
        sbs.oldLineNumber = oldNum++;
        sbs.oldText = line;
        sbs.oldType = LineType::Context;
        sbs.newLineNumber = newNum++;
        sbs.newText = line;
        sbs.newType = LineType::Context;
        result.sideBySideRows.append(sbs);

        UnifiedRow uni;
        uni.oldLineNumber = sbs.oldLineNumber;
        uni.newLineNumber = sbs.newLineNumber;
        uni.text = line;
        uni.type = LineType::Context;
        result.unifiedRows.append(uni);
    }

    // 2. Local (deleted / left) and Remote (added / right)
    result.deletedCount += localLines.size();
    result.addedCount += remoteLines.size();

    int pairCount = std::min(localLines.size(), remoteLines.size());
    QVector<QVector<TextSpan>> delSpans(localLines.size());
    QVector<QVector<TextSpan>> addSpans(remoteLines.size());
    for (int p = 0; p < pairCount; ++p)
    {
        computeWordDiff(localLines[p], remoteLines[p], delSpans[p], addSpans[p]);
    }
    for (int d = pairCount; d < localLines.size(); ++d)
    {
        if (!localLines[d].isEmpty())
            delSpans[d].append({0, (int)localLines[d].length(), true});
    }
    for (int a = pairCount; a < remoteLines.size(); ++a)
    {
        if (!remoteLines[a].isEmpty())
            addSpans[a].append({0, (int)remoteLines[a].length(), true});
    }

    for (int d = 0; d < localLines.size(); ++d)
    {
        UnifiedRow uni;
        uni.oldLineNumber = oldNum + d;
        uni.newLineNumber = -1;
        uni.text = localLines[d];
        uni.spans = delSpans[d];
        uni.type = LineType::Deleted;
        result.unifiedRows.append(uni);
    }
    for (int a = 0; a < remoteLines.size(); ++a)
    {
        UnifiedRow uni;
        uni.oldLineNumber = -1;
        uni.newLineNumber = newNum + a;
        uni.text = remoteLines[a];
        uni.spans = addSpans[a];
        uni.type = LineType::Added;
        result.unifiedRows.append(uni);
    }

    int maxRows = std::max(localLines.size(), remoteLines.size());
    for (int r = 0; r < maxRows; ++r)
    {
        SideBySideRow sbs;
        if (r < localLines.size())
        {
            sbs.oldLineNumber = oldNum + r;
            sbs.oldText = localLines[r];
            sbs.oldSpans = delSpans[r];
            sbs.oldType = LineType::Deleted;
        }
        else
        {
            sbs.oldType = LineType::Empty;
        }

        if (r < remoteLines.size())
        {
            sbs.newLineNumber = newNum + r;
            sbs.newText = remoteLines[r];
            sbs.newSpans = addSpans[r];
            sbs.newType = LineType::Added;
        }
        else
        {
            sbs.newType = LineType::Empty;
        }
        result.sideBySideRows.append(sbs);
    }
    oldNum += localLines.size();
    newNum += remoteLines.size();

    // 3. Context after hunk
    for (const auto &line : afterLines)
    {
        SideBySideRow sbs;
        sbs.oldLineNumber = oldNum++;
        sbs.oldText = line;
        sbs.oldType = LineType::Context;
        sbs.newLineNumber = newNum++;
        sbs.newText = line;
        sbs.newType = LineType::Context;
        result.sideBySideRows.append(sbs);

        UnifiedRow uni;
        uni.oldLineNumber = sbs.oldLineNumber;
        uni.newLineNumber = sbs.newLineNumber;
        uni.text = line;
        uni.type = LineType::Context;
        result.unifiedRows.append(uni);
    }

    return result;
}

ParsedDiff diffTexts(const QString &oldText, const QString &newText, int startLine)
{
    ParsedDiff result;
    if (oldText == newText)
    {
        if (oldText.isEmpty())
            return result;
        QString s = oldText;
        s.replace("\r\n", "\n").replace('\r', '\n');
        QStringList lines = s.split('\n');
        if (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        int lineNum = startLine;
        for (const auto &line : lines)
        {
            SideBySideRow sbs;
            sbs.oldLineNumber = lineNum;
            sbs.newLineNumber = lineNum++;
            sbs.oldText = line;
            sbs.newText = line;
            sbs.oldType = LineType::Context;
            sbs.newType = LineType::Context;
            result.sideBySideRows.append(sbs);

            UnifiedRow uni;
            uni.oldLineNumber = sbs.oldLineNumber;
            uni.newLineNumber = sbs.newLineNumber;
            uni.text = line;
            uni.type = LineType::Context;
            result.unifiedRows.append(uni);
        }
        return result;
    }

    auto splitLines = [](const QString &text) {
        if (text.isEmpty())
            return QStringList();
        QString s = text;
        s.replace("\r\n", "\n").replace('\r', '\n');
        QStringList lines = s.split('\n');
        if (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
        return lines;
    };

    const QStringList oldLines = splitLines(oldText);
    const QStringList newLines = splitLines(newText);
    const int n = oldLines.size();
    const int m = newLines.size();

    int prefix = 0;
    while (prefix < n && prefix < m && oldLines[prefix] == newLines[prefix])
        ++prefix;

    int suffix = 0;
    while (suffix < (n - prefix) && suffix < (m - prefix) &&
           oldLines[n - 1 - suffix] == newLines[m - 1 - suffix])
        ++suffix;

    int oldNum = startLine;
    int newNum = startLine;

    for (int i = 0; i < prefix; ++i)
    {
        SideBySideRow sbs;
        sbs.oldLineNumber = oldNum++;
        sbs.newLineNumber = newNum++;
        sbs.oldText = oldLines[i];
        sbs.newText = newLines[i];
        sbs.oldType = LineType::Context;
        sbs.newType = LineType::Context;
        result.sideBySideRows.append(sbs);

        UnifiedRow uni;
        uni.oldLineNumber = sbs.oldLineNumber;
        uni.newLineNumber = sbs.newLineNumber;
        uni.text = oldLines[i];
        uni.type = LineType::Context;
        result.unifiedRows.append(uni);
    }

    const int midN = n - prefix - suffix;
    const int midM = m - prefix - suffix;

    if (midN > 0 || midM > 0)
    {
        QVector<bool> oldMatched(midN, false);
        QVector<bool> newMatched(midM, false);

        if (midN > 0 && midM > 0 && (qint64)midN * midM <= 2000 * 2000)
        {
            QVector<QVector<int>> dp(midN + 1, QVector<int>(midM + 1, 0));
            for (int i = 1; i <= midN; ++i)
            {
                for (int j = 1; j <= midM; ++j)
                {
                    if (oldLines[prefix + i - 1] == newLines[prefix + j - 1])
                        dp[i][j] = dp[i - 1][j - 1] + 1;
                    else
                        dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
                }
            }
            int i = midN, j = midM;
            while (i > 0 && j > 0)
            {
                if (oldLines[prefix + i - 1] == newLines[prefix + j - 1])
                {
                    oldMatched[i - 1] = true;
                    newMatched[j - 1] = true;
                    --i;
                    --j;
                }
                else if (dp[i - 1][j] >= dp[i][j - 1])
                {
                    --i;
                }
                else
                {
                    --j;
                }
            }
        }

        int oi = 0, ni = 0;
        while (oi < midN || ni < midM)
        {
            if (oi < midN && ni < midM && oldMatched[oi] && newMatched[ni])
            {
                SideBySideRow sbs;
                sbs.oldLineNumber = oldNum++;
                sbs.newLineNumber = newNum++;
                sbs.oldText = oldLines[prefix + oi];
                sbs.newText = newLines[prefix + ni];
                sbs.oldType = LineType::Context;
                sbs.newType = LineType::Context;
                result.sideBySideRows.append(sbs);

                UnifiedRow uni;
                uni.oldLineNumber = sbs.oldLineNumber;
                uni.newLineNumber = sbs.newLineNumber;
                uni.text = oldLines[prefix + oi];
                uni.type = LineType::Context;
                result.unifiedRows.append(uni);

                ++oi;
                ++ni;
            }
            else
            {
                QVector<int> delIndices;
                QVector<int> addIndices;
                while (oi < midN && !oldMatched[oi])
                {
                    delIndices.append(prefix + oi);
                    ++oi;
                }
                while (ni < midM && !newMatched[ni])
                {
                    addIndices.append(prefix + ni);
                    ++ni;
                }

                result.deletedCount += delIndices.size();
                result.addedCount += addIndices.size();

                int pairCount = std::min(delIndices.size(), addIndices.size());
                QVector<QVector<TextSpan>> delSpans(delIndices.size());
                QVector<QVector<TextSpan>> addSpans(addIndices.size());

                for (int p = 0; p < pairCount; ++p)
                {
                    computeWordDiff(oldLines[delIndices[p]], newLines[addIndices[p]],
                                    delSpans[p], addSpans[p]);
                }
                for (int d = pairCount; d < delIndices.size(); ++d)
                {
                    if (!oldLines[delIndices[d]].isEmpty())
                        delSpans[d].append({0, (int)oldLines[delIndices[d]].length(), true});
                }
                for (int a = pairCount; a < addIndices.size(); ++a)
                {
                    if (!newLines[addIndices[a]].isEmpty())
                        addSpans[a].append({0, (int)newLines[addIndices[a]].length(), true});
                }

                for (int d = 0; d < delIndices.size(); ++d)
                {
                    UnifiedRow uni;
                    uni.oldLineNumber = oldNum + d;
                    uni.newLineNumber = -1;
                    uni.text = oldLines[delIndices[d]];
                    uni.spans = delSpans[d];
                    uni.type = LineType::Deleted;
                    result.unifiedRows.append(uni);
                }
                for (int a = 0; a < addIndices.size(); ++a)
                {
                    UnifiedRow uni;
                    uni.oldLineNumber = -1;
                    uni.newLineNumber = newNum + a;
                    uni.text = newLines[addIndices[a]];
                    uni.spans = addSpans[a];
                    uni.type = LineType::Added;
                    result.unifiedRows.append(uni);
                }

                int maxRows = std::max(delIndices.size(), addIndices.size());
                for (int r = 0; r < maxRows; ++r)
                {
                    SideBySideRow sbs;
                    if (r < delIndices.size())
                    {
                        sbs.oldLineNumber = oldNum + r;
                        sbs.oldText = oldLines[delIndices[r]];
                        sbs.oldSpans = delSpans[r];
                        sbs.oldType = LineType::Deleted;
                    }
                    else
                    {
                        sbs.oldType = LineType::Empty;
                    }

                    if (r < addIndices.size())
                    {
                        sbs.newLineNumber = newNum + r;
                        sbs.newText = newLines[addIndices[r]];
                        sbs.newSpans = addSpans[r];
                        sbs.newType = LineType::Added;
                    }
                    else
                    {
                        sbs.newType = LineType::Empty;
                    }
                    result.sideBySideRows.append(sbs);
                }

                oldNum += delIndices.size();
                newNum += addIndices.size();
            }
        }
    }

    for (int i = n - suffix; i < n; ++i)
    {
        SideBySideRow sbs;
        sbs.oldLineNumber = oldNum++;
        sbs.newLineNumber = newNum++;
        sbs.oldText = oldLines[i];
        sbs.newText = newLines[m - n + i];
        sbs.oldType = LineType::Context;
        sbs.newType = LineType::Context;
        result.sideBySideRows.append(sbs);

        UnifiedRow uni;
        uni.oldLineNumber = sbs.oldLineNumber;
        uni.newLineNumber = sbs.newLineNumber;
        uni.text = oldLines[i];
        uni.type = LineType::Context;
        result.unifiedRows.append(uni);
    }

    return result;
}

QString renderHtml(const ParsedDiff &diff, ViewMode mode, const RenderColors &colors, const QString &fontFamily,
                   const QString &oldHeader, const QString &newHeader)
{
    if (diff.isBinary)
    {
        return QString(
            "<html><head><style>"
            "body { font-family: %1; background-color: %2; color: %3; margin: 24px; text-align: center; }"
            ".notice-box { margin-top: 60px; }"
            ".title { font-size: 15px; font-weight: 600; margin-bottom: 8px; }"
            ".desc { font-size: 13px; color: %4; }"
            "</style></head><body>"
            "<div class=\"notice-box\">"
            "<div class=\"title\">二进制文件变更</div>"
            "<div class=\"desc\">%5</div>"
            "</div></body></html>")
            .arg(fontFamily, colors.canvas.name(), colors.text.name(),
                 colors.secondaryText.name(),
                 diff.binaryNotice.toHtmlEscaped());
    }

    if (diff.sideBySideRows.isEmpty() && diff.unifiedRows.isEmpty())
    {
        return QString(
            "<html><head><style>"
            "body { font-family: %1; background-color: %2; color: %3; margin: 24px; text-align: center; }"
            ".empty { font-size: 13px; color: %4; margin-top: 48px; }"
            "</style></head><body>"
            "<div class=\"empty\">当前文件没有可显示的文本变更。</div>"
            "</body></html>")
            .arg(fontFamily, colors.canvas.name(), colors.secondaryText.name(), colors.secondaryText.name());
    }

    QString html;
    html.reserve(32768);

    const bool dark = colors.canvas.lightness() < 128;
    const QString delBg = dark ? "#3d1c21" : "#ffebe9";
    const QString delWordBg = dark ? "#6a252f" : "#ffc1c0";
    const QString delText = dark ? "#ff9492" : "#b62324";

    const QString addBg = dark ? "#163824" : "#dafbe1";
    const QString addWordBg = dark ? "#245c38" : "#aceebb";
    const QString addText = dark ? "#85e89d" : "#1a7f37";

    const QString emptyBg = dark ? "#1c2128" : "#f6f8fa";
    const QString headerBg = dark ? "#21262d" : "#f1f3f5";
    const QString borderColor = dark ? "#30363d" : "#d0d7de";
    const QString numColor = dark ? "#768390" : "#656d76";
    const QString textColor = dark ? "#e6edf3" : "#24292f";

    html += QString(
        "<html><head><style>"
        "body { font-family: %1; background-color: %2; color: %3; margin: 0; padding: 0; }"
        "table { width: 100%; border-collapse: collapse; table-layout: fixed; font-size: 12px; line-height: 1.6; }"
        "th { font-size: 11px; font-weight: 600; padding: 7px 10px; background-color: %4; color: %5; border-bottom: 2px solid %6; text-align: left; user-select: none; }"
        "td { padding: 4px 8px; vertical-align: top; white-space: pre-wrap; word-break: break-all; }"
        ".num { width: 36px; text-align: right; user-select: none; color: %7; font-size: 11px; padding-right: 8px; border-right: 1px solid %6; background-color: %4; }"
        ".header-cell { background-color: %4; color: %5; font-weight: 500; font-size: 11px; padding: 4px 12px; text-align: center; border-top: 1px dashed %6; border-bottom: 1px dashed %6; letter-spacing: 2px; user-select: none; }"
        ".del-num { background-color: %8; color: %9; border-right: 1px solid %6; }"
        ".del-text { background-color: %8; color: %9; }"
        ".add-num { background-color: %10; color: %11; border-right: 1px solid %6; }"
        ".add-text { background-color: %10; color: %11; }"
        ".empty-cell { background-color: %12; color: transparent; user-select: none; }"
        ".col-divider { border-right: 1px solid %6; }"
        "</style></head><body><table width=\"100%\" border=\"0\" cellspacing=\"0\" cellpadding=\"0\">"
        "<tr class=\"top-bar\"><th colspan=\"2\" class=\"col-divider\">%13</th><th colspan=\"2\">%14</th></tr>")
        .arg(fontFamily, colors.canvas.name(), textColor,
             headerBg, colors.secondaryText.name(), borderColor,
             numColor, delBg, delText, addBg, addText, emptyBg)
        .arg(oldHeader.toHtmlEscaped(), newHeader.toHtmlEscaped());

    if (mode == ViewMode::SideBySide)
    {
        for (const auto &row : diff.sideBySideRows)
        {
            if (row.isHeader)
            {
                html += QString("<tr><td colspan=\"4\" class=\"header-cell\">%1</td></tr>")
                            .arg(row.headerText.toHtmlEscaped());
                continue;
            }

            html += "<tr>";

            // 左栏 (旧版本)
            if (row.oldType == LineType::Empty)
            {
                html += "<td width=\"36\" class=\"num empty-cell\"></td><td width=\"45%\" class=\"empty-cell col-divider\"></td>";
            }
            else
            {
                QString numStr = row.oldLineNumber > 0 ? QString::number(row.oldLineNumber) : QString();
                bool isDel = (row.oldType == LineType::Deleted);
                QString numCls = isDel ? "num del-num" : "num";
                QString textCls = isDel ? "del-text col-divider" : "col-divider";
                QString cellHtml = spansToHtml(row.oldText, row.oldSpans, delWordBg);
                html += QString("<td width=\"36\" class=\"%1\">%2</td><td width=\"45%\" class=\"%3\">%4</td>")
                            .arg(numCls, numStr, textCls, cellHtml);
            }

            // 右栏 (新版本)
            if (row.newType == LineType::Empty)
            {
                html += "<td width=\"36\" class=\"num empty-cell\"></td><td width=\"45%\" class=\"empty-cell\"></td>";
            }
            else
            {
                QString numStr = row.newLineNumber > 0 ? QString::number(row.newLineNumber) : QString();
                bool isAdd = (row.newType == LineType::Added);
                QString numCls = isAdd ? "num add-num" : "num";
                QString textCls = isAdd ? "add-text" : "";
                QString cellHtml = spansToHtml(row.newText, row.newSpans, addWordBg);
                html += QString("<td width=\"36\" class=\"%1\">%2</td><td width=\"45%\" class=\"%3\">%4</td>")
                            .arg(numCls, numStr, textCls, cellHtml);
            }

            html += "</tr>";
        }
    }
    else // Unified
    {
        for (const auto &row : diff.unifiedRows)
        {
            if (row.isHeader)
            {
                html += QString("<tr><td colspan=\"3\" class=\"header-cell\">%1</td></tr>")
                            .arg(row.headerText.toHtmlEscaped());
                continue;
            }

            QString oldNumStr = row.oldLineNumber > 0 ? QString::number(row.oldLineNumber) : QString();
            QString newNumStr = row.newLineNumber > 0 ? QString::number(row.newLineNumber) : QString();

            QString numCls = "num";
            QString textCls;
            QString wordBg;
            if (row.type == LineType::Deleted)
            {
                numCls = "num del-num";
                textCls = "del-text";
                wordBg = delWordBg;
            }
            else if (row.type == LineType::Added)
            {
                numCls = "num add-num";
                textCls = "add-text";
                wordBg = addWordBg;
            }

            QString cellHtml = spansToHtml(row.text, row.spans, wordBg);

            html += QString("<tr><td width=\"36\" class=\"%1\">%2</td><td width=\"36\" class=\"%1\">%3</td><td class=\"%4\">%5</td></tr>")
                        .arg(numCls, oldNumStr, newNumStr, textCls, cellHtml);
        }
    }

    html += "</table></body></html>";
    return html;
}

} // namespace DiffParser
