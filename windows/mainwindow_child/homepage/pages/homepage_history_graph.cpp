#include "homepage_history_graph.h"
#include "windows/mainwindow_presentation.h"
#include <QPainterPath>
#include <QSet>

namespace HistoryGraph
{
QColor color(int index)
{
    const bool dark = UiStyle::colors().canvas.lightness() < 128;
    const QVector<QColor> palette = dark
        ? QVector<QColor>{"#f58a3c", "#e5b844", "#dc72ac", "#72a8ef", "#63c1ad", "#b598ec"}
        : QVector<QColor>{"#bb571b", "#a57c0c", "#b53778", "#366bb5", "#267e6b", "#8055b4"};
    return palette[qMax(0, index) % palette.size()];
}
void populate(QStandardItemModel &model, const QVector<Revision> &revisions, const QString &currentCommit)
{
    struct Lane { QString oid; int color; QStringList refs; };
    QVector<Lane> lanes;
    int nextColor = 0, maxLanes = 1;
    const auto find = [&lanes](const QString &oid) {
        for (int i = 0; i < lanes.size(); ++i) if (lanes[i].oid == oid) return i;
        return -1;
    };
    for (int row = 0; row < revisions.size(); ++row)
    {
        const auto &revision = revisions[row];
        const bool incoming = find(revision.hash) >= 0;
        if (!incoming) lanes.append({revision.hash, nextColor++, revision.branchRefs});
        const int node = find(revision.hash), nodeColor = lanes[node].color;
        for (const auto &ref : revision.branchRefs) if (!lanes[node].refs.contains(ref)) lanes[node].refs.append(ref);
        const auto before = lanes;
        lanes.removeAt(node);
        int insertion = qMin(node, int(lanes.size()));
        for (int p = 0; p < revision.parents.size(); ++p)
        {
            const int parent = find(revision.parents[p]);
            if (parent < 0) lanes.insert(insertion++, {revision.parents[p], p == 0 ? nodeColor : nextColor++, before[node].refs});
            else for (const auto &ref : before[node].refs)
                if (!lanes[parent].refs.contains(ref)) lanes[parent].refs.append(ref);
        }
        QVariantList edges;
        for (int i = 0; i < before.size(); ++i)
        {
            if (i == node)
                for (int p = 0; p < revision.parents.size(); ++p)
                {
                    const int target = find(revision.parents[p]);
                    edges.append(QVariant(QVariantList{i, target, p == 0 ? nodeColor : lanes[target].color, before[node].refs}));
                }
            else edges.append(QVariant(QVariantList{i, find(before[i].oid), before[i].color, before[i].refs}));
        }
        auto *item = model.item(row, 0);
        item->setData(node, Node); item->setData(edges, Edges); item->setData(incoming, Incoming);
        item->setData(nodeColor, Color); item->setData(revision.parents.size() > 1, Merge);
        item->setData(revision.hash == currentCommit, Current); item->setData(revision.refs, Refs);
        item->setData(revision.branchRefs, TipRefs);
        item->setData(before.value(node).refs, NodeRefs);
        QVariantList laneRefs; for (const auto &lane : before) laneRefs.append(lane.refs);
        item->setData(laneRefs, LaneRefs);
        maxLanes = qMax(maxLanes, int(qMax(before.size(), lanes.size())));
    }
    for (int row = 0; row < model.rowCount(); ++row) model.item(row, 0)->setData(24 + maxLanes * 18, Width);
}
void insertCommonAncestors(QStandardItemModel &model, int row, int count, bool expanded)
{
    if (count <= 0) return;
    const auto previous = model.index(row - 1, 0);
    const auto next = model.index(row, 0);
    QVariantList incoming;
    QSet<int> lanes;
    for (const auto &value : previous.data(Edges).toList())
    {
        const auto edge = value.toList();
        if (edge.size() < 3 || lanes.contains(edge[1].toInt())) continue;
        lanes.insert(edge[1].toInt());
        incoming.append(QVariant(QVariantList{edge[1], 0, edge[2]}));
    }
    const auto label = QString("%1 个共同祖先").arg(count);
    const auto operation = expanded ? QStringLiteral("收起") : QStringLiteral("展开");
    QList<QStandardItem *> items;
    for (int column = 0; column < model.columnCount(); ++column)
    {
        auto *item = new QStandardItem(column == 0 ? label : QString());
        item->setEditable(false);
        item->setData(count, AncestorCount);
        item->setData(expanded, AncestorsExpanded);
        item->setData(label, Qt::AccessibleTextRole);
        item->setData("点击或按 Enter " + operation + "共同祖先，仅查看历史，不切换方案。", Qt::AccessibleDescriptionRole);
        item->setToolTip(operation + "共同祖先的历史版本");
        items.append(item);
    }
    items.first()->setData(incoming, Edges);
    items.first()->setData((previous.isValid() ? previous : next).data(Color).toInt(), Color);
    items.first()->setData(qMax(42, qMax(previous.data(Width).toInt(), next.data(Width).toInt())), Width);
    // With no unique versions, the group supplies the first commit's incoming line.
    if (expanded && row == 0 && next.isValid()) model.item(row, 0)->setData(true, Incoming);
    model.insertRow(row, items);
}
QStringList hit(const QRect &rect, const QModelIndex &index, const QPoint &position)
{
    if (!index.isValid() || index.column() != 0 || index.data(AncestorCount).toInt() > 0 || !rect.contains(position)) return {};
    const int lane = qRound((position.x() - (rect.left() + 14)) / 18.0);
    if (lane < 0) return {};
    const auto lanes = index.data(LaneRefs).toList();
    if (lane < lanes.size()) {
        const auto refs = lanes[lane].toStringList();
        if (qAbs(position.x() - (rect.left() + 14 + lane * 18)) <= 8) return refs.isEmpty() ? QStringList{"HEAD"} : refs;
    }
    for (const auto &value : index.data(Edges).toList()) {
        const auto edge = value.toList(); if (edge.size() < 4) continue;
        const int from = edge[0].toInt(), to = edge[1].toInt();
        const int y0 = from == index.data(Node).toInt() ? rect.center().y() : rect.top();
        const int y1 = rect.bottom() + 1;
        const double t = qBound(0.0, (position.y() - y0) / double(qMax(1, y1 - y0)), 1.0);
        const double x0 = rect.left() + 14 + from * 18, x1 = rect.left() + 14 + to * 18;
        const double x = x0 + (x1 - x0) * t;
        if (qAbs(position.x() - x) <= 7)
        {
            const auto refs = edge[3].toStringList();
            return refs.isEmpty() ? QStringList{"HEAD"} : refs;
        }
    }
    return {};
}
void paint(QPainter *p, const QRect &rect, const QModelIndex &index)
{
    const int node = index.data(Node).toInt();
    const auto x = [&](int lane) { return rect.left() + 14 + lane * 18; };
    const auto nodeColor = color(index.data(Color).toInt());
    const int middle = rect.center().y(), bottom = rect.bottom() + 1;
    p->save(); p->setRenderHint(QPainter::Antialiasing);
    if (index.data(AncestorCount).toInt() > 0)
    {
        const bool expanded = index.data(AncestorsExpanded).toBool();
        // Several unfinished lanes can lead into this aggregate, not a Git commit.
        for (const auto &value : index.data(Edges).toList())
        {
            const auto edge = value.toList();
            if (expanded)
            {
                p->setPen(QPen(color(edge[2].toInt()), 1.7));
                p->drawLine(x(edge[0].toInt()), rect.top(), x(edge[0].toInt()), bottom);
                continue;
            }
            p->setPen(QPen(color(edge[2].toInt()), 1.7, Qt::DashLine, Qt::RoundCap));
            QPainterPath line(QPointF(x(edge[0].toInt()), rect.top()));
            line.cubicTo(x(edge[0].toInt()), middle - 10, x(0), middle - 14, x(0), middle - 7);
            p->drawPath(line);
        }
        if (expanded && index.data(Edges).toList().isEmpty())
        {
            p->setPen(QPen(nodeColor, 1.7));
            p->drawLine(x(0), middle, x(0), bottom);
        }
        p->setPen(QPen(nodeColor, 1.5));
        p->setBrush(UiStyle::colors().canvas);
        p->drawEllipse(QPoint(x(0), middle), 7, 7);
        p->setPen(QPen(nodeColor, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const int direction = expanded ? -1 : 1;
        p->drawPolyline(QPolygonF{QPointF(x(0) - 3, middle - direction), QPointF(x(0), middle + 2 * direction), QPointF(x(0) + 3, middle - direction)});
        p->restore();
        return;
    }
    p->setPen(QPen(nodeColor, 1.7));
    if (index.data(Incoming).toBool()) p->drawLine(x(node), rect.top(), x(node), middle);
    for (const auto &value : index.data(Edges).toList())
    {
        const auto edge = value.toList();
        const int from = edge[0].toInt(), to = edge[1].toInt();
        const int start = from == node ? middle : rect.top();
        p->setPen(QPen(color(edge[2].toInt()), 1.7));
        QPainterPath line(QPointF(x(from), start));
        if (from == to) line.lineTo(x(to), bottom);
        else line.cubicTo(x(from), start + (bottom - start) * .65, x(to), start + (bottom - start) * .35, x(to), bottom);
        p->drawPath(line);
    }
    p->setPen(QPen(nodeColor, 1.8));
    const bool ring = index.data(Merge).toBool() || index.data(Current).toBool();
    p->setBrush(ring ? UiStyle::colors().canvas : nodeColor);
    const int radius = ring ? 5 : 3;
    p->drawEllipse(QPoint(x(node), middle), radius, radius);
    if (ring) { p->setPen(Qt::NoPen); p->setBrush(nodeColor); p->drawEllipse(QPoint(x(node), middle), 2, 2); }
    p->restore();
}
}
