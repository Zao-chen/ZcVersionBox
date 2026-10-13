#include "code_text_style.h"

namespace CodeTextStyle
{
Colors colors(const QColor &canvas)
{
    const bool dark = canvas.lightness() < 128;
    return {canvas,
            QColor(dark ? "#e6edf3" : "#24292f"),
            QColor(dark ? "#21262d" : "#f1f3f5"),
            QColor(dark ? "#768390" : "#656d76"),
            QColor(dark ? "#30363d" : "#d0d7de")};
}

QFont font(const QFont &base, bool lineNumber)
{
    auto result = base;
    result.setPixelSize(lineNumber ? LineNumberFontSize : FontSize);
    return result;
}

QString htmlStyle(const Colors &colors, const QString &fontFamily)
{
    return QString(
        "body { font-family: %1; background-color: %2; color: %3; margin: 0; padding: 0; }")
        .arg(fontFamily, colors.canvas.name(), colors.text.name())
        + QString(
            "table { width: 100%; border-collapse: collapse; table-layout: fixed; font-size: %1px; line-height: 1.6; }"
            "td { padding: %2px %3px; vertical-align: top; white-space: pre-wrap; word-break: break-all; }"
            ".num { width: %4px; text-align: right; user-select: none; color: %5; font-size: %6px; padding-right: %3px; border-right: %7px solid %8; background-color: %9; }")
            .arg(FontSize).arg(VerticalPadding).arg(HorizontalPadding).arg(MinimumGutterWidth)
            .arg(colors.lineNumber.name()).arg(LineNumberFontSize).arg(BorderWidth)
            .arg(colors.border.name(), colors.gutter.name());
}
} // namespace CodeTextStyle
