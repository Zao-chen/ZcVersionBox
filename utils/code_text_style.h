#pragma once
#include <QColor>
#include <QFont>
#include <QString>

// Shared presentation for native source editors and the HTML diff renderer.
// Keep this independent of Widgets so rendering can also run in zc_services.
namespace CodeTextStyle
{
inline constexpr int FontSize = 12;
inline constexpr int LineNumberFontSize = 11;
inline constexpr int HorizontalPadding = 8;
inline constexpr int VerticalPadding = 4;
inline constexpr int MinimumGutterWidth = 36;
inline constexpr int BorderWidth = 1;

struct Colors
{
    QColor canvas;
    QColor text;
    QColor gutter;
    QColor lineNumber;
    QColor border;
};

Colors colors(const QColor &canvas);
QFont font(const QFont &base, bool lineNumber = false);
QString htmlStyle(const Colors &colors, const QString &fontFamily);
} // namespace CodeTextStyle
