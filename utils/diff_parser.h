#pragma once
#include <QColor>
#include <QString>
#include <QVector>

namespace DiffParser
{

enum class LineType
{
    Context,   // 未变动的上下文行
    Added,     // 新增行
    Deleted,   // 删除行
    Header,    // Hunk 标头说明（例如 "第 14 行附近"）
    Empty      // 并排对齐时的空白占位
};

struct TextSpan
{
    int start{0};
    int length{0};
    bool highlighted{false};
};

struct SideBySideRow
{
    // 左栏 (旧版本 / 修改前)
    int oldLineNumber{-1};
    QString oldText;
    QVector<TextSpan> oldSpans;
    LineType oldType{LineType::Empty};

    // 右栏 (新版本 / 修改后)
    int newLineNumber{-1};
    QString newText;
    QVector<TextSpan> newSpans;
    LineType newType{LineType::Empty};

    bool isHeader{false};
    QString headerText;
};

struct UnifiedRow
{
    int oldLineNumber{-1};
    int newLineNumber{-1};
    QString text;
    QVector<TextSpan> spans;
    LineType type{LineType::Context};
    bool isHeader{false};
    QString headerText;
};

struct ParsedDiff
{
    bool isBinary{false};
    QString binaryNotice;
    int addedCount{0};
    int deletedCount{0};
    QVector<SideBySideRow> sideBySideRows;
    QVector<UnifiedRow> unifiedRows;
    QString rawPatch;
};

// 解析 Git unified diff 格式文本
ParsedDiff parse(const QString &rawDiff);

// 生成富文本 HTML 视图 (支持 SideBySide 和 Unified)
enum class ViewMode
{
    SideBySide,
    Unified
};

struct RenderColors
{
    QColor canvas;
    QColor surface;
    QColor text;
    QColor secondaryText;
    QColor border;
    QColor addedBg;
    QColor addedText;
    QColor addedWordBg;
    QColor removedBg;
    QColor removedText;
    QColor removedWordBg;
    QColor headerBg;
    QColor emptyBg;
};

QString renderHtml(const ParsedDiff &diff, ViewMode mode, const RenderColors &colors, const QString &fontFamily);

} // namespace DiffParser
