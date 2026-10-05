#pragma once
#include "windows/mainwindow_presentation.h"
#include <QPainter>
#include <QStyledItemDelegate>

enum DiffFileRole
{
    PathRole = Qt::UserRole + 1,
    StatusRole,
    SummaryRole
};

inline QString statusText(const QString &status)
{
    if (status.isEmpty())
        return QStringLiteral("变更");
    if (status == QLatin1String("!") || status == QStringLiteral("冲突"))
        return QStringLiteral("冲突");
    if (status == QLatin1String("✓") || status == QStringLiteral("已处理"))
        return QStringLiteral("已处理");
    if (status == QLatin1String("=") || status == QStringLiteral("保留"))
        return QStringLiteral("保留");
    if (status == QLatin1String("-") || status == QStringLiteral("保持删除"))
        return QStringLiteral("保持删除");
    if (status == QStringLiteral("新增"))
        return QStringLiteral("新增");
    if (status == QStringLiteral("删除"))
        return QStringLiteral("删除");
    if (status == QStringLiteral("修改"))
        return QStringLiteral("修改");

    const QMap<QChar, QString> labels{{'A', "新增"}, {'D', "删除"}, {'M', "修改"}, {'R', "重命名"}, {'C', "复制"}, {'T', "类型变化"}};
    return labels.value(status.front(), status);
}

inline QColor statusBadgeColor(const QString &status, const UiStyle::Colors &colors)
{
    if (status.startsWith('A') || status == QStringLiteral("新增") || status == QLatin1String("✓") || status == QStringLiteral("已处理"))
        return colors.added;
    if (status.startsWith('D') || status == QStringLiteral("删除") || status == QLatin1String("!") || status == QStringLiteral("冲突") || status == QLatin1String("-") || status == QStringLiteral("保持删除"))
        return colors.removed;
    if (status.startsWith('M') || status == QStringLiteral("修改"))
        return QColor(220, 130, 20); // 暖橙色
    if (status.startsWith('R') || status.startsWith('C') || status == QStringLiteral("重命名") || status == QStringLiteral("复制"))
        return QColor(30, 136, 229); // 科技蓝
    return colors.secondary;
}

class DiffFileDelegate : public QStyledItemDelegate
{
  public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return {0, UiStyle::rowHeight(58, UiStyle::font(UiStyle::FontRole::Body), true)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        const auto colors = UiStyle::colors();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const auto row = opt.rect.adjusted(0, 2, 0, -2);
        if (opt.state.testFlag(QStyle::State_Selected) || opt.state.testFlag(QStyle::State_MouseOver))
        {
            painter->setPen(Qt::NoPen);
            painter->setBrush(opt.state.testFlag(QStyle::State_Selected) ? colors.selected : colors.hover);
            painter->drawRoundedRect(row, 8, 8);
        }
        if (UiStyle::isKeyboardNavigationActive() && opt.state.testFlag(QStyle::State_HasFocus))
        {
            painter->setPen(colors.secondary);
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(row).adjusted(.5, .5, -.5, -.5), 8, 8);
        }

        const auto font = UiStyle::font(UiStyle::FontRole::Body);
        const auto caption = UiStyle::font(UiStyle::FontRole::Caption);
        const auto textRect = row.adjusted(12, 7, -12, -7);

        // 状态徽章 (Badge)
        const auto status = index.data(StatusRole).toString();
        const auto badgeName = statusText(status);
        const auto badgeCol = statusBadgeColor(status, colors);

        QFont badgeFont = caption;
        badgeFont.setPointSize(caption.pointSize() > 2 ? caption.pointSize() - 1 : 9);
        QFontMetrics badgeFm(badgeFont);
        const int badgeTextW = badgeFm.horizontalAdvance(badgeName);
        const int badgeW = badgeTextW + 10;
        const int badgeH = badgeFm.height() + 4;
        const QRect badgeRect(textRect.right() - badgeW, textRect.y() + 1, badgeW, badgeH);

        QColor badgeBg = badgeCol;
        badgeBg.setAlpha(36);
        painter->setPen(Qt::NoPen);
        painter->setBrush(badgeBg);
        painter->drawRoundedRect(badgeRect, 4, 4);

        painter->setFont(badgeFont);
        painter->setPen(badgeCol);
        painter->drawText(badgeRect, Qt::AlignCenter, badgeName);

        // 主标题：文件名
        const int titleW = textRect.width() - badgeW - 8;
        painter->setFont(font);
        painter->setPen(colors.text);
        painter->drawText(QRect(textRect.x(), textRect.y(), titleW, QFontMetrics(font).height()),
                          Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, titleW));

        // 副标题：变更统计或处理进度
        const auto summary = index.data(SummaryRole).toString();
        const QString subText = (!summary.isEmpty() && summary != "-") ? summary : QString();

        if (!subText.isEmpty())
        {
            painter->setFont(caption);
            painter->setPen(colors.secondary);
            painter->drawText(QRect(textRect.x(), textRect.y() + QFontMetrics(font).height() + 3, textRect.width(), QFontMetrics(caption).height()),
                              Qt::AlignVCenter | Qt::AlignLeft,
                              subText);
        }

        painter->restore();
    }
};
