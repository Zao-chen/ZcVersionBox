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
QVector<Revision> userFacing(QVector<Revision> revisions)
{
    for (auto &revision : revisions)
    {
        QStringList names, refs;
        for (int i = 0; i < revision.branchRefs.size(); ++i)
        {
            const auto &ref = revision.branchRefs[i];
            auto name = i < revision.refs.size() ? revision.refs[i] : ref;
            if (ref.startsWith("refs/remotes/"))
            {
                const auto slash = ref.indexOf('/', 13);
                const auto branch = slash < 0 ? QString() : ref.mid(slash + 1);
                if (!branch.isEmpty() && revision.branchRefs.contains("refs/heads/" + branch)) continue;
                if (!branch.isEmpty()) name = branch + "（云端）";
            }
            names.append(name);
            refs.append(ref);
        }
        revision.refs = names;
        revision.branchRefs = refs;
    }
    return revisions;
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
    const auto label = QString("更早的 %1 个版本（与其他方案共有）").arg(count);
    const auto operation = expanded ? QStringLiteral("收起") : QStringLiteral("展开");
    QList<QStandardItem *> items;
    for (int column = 0; column < model.columnCount(); ++column)
    {
        auto *item = new QStandardItem(column == 0 ? label : QString());
        item->setEditable(false);
        item->setData(count, AncestorCount);
        item->setData(expanded, AncestorsExpanded);
        item->setData(label, Qt::AccessibleTextRole);
        item->setData("点击或按 Enter " + operation + "更早的版本，仅查看历史，不切换方案。", Qt::AccessibleDescriptionRole);
        item->setToolTip(operation + "与其他方案共有的更早版本");
        items.append(item);
    }
    items.first()->setData(incoming, Edges);
    items.first()->setData((previous.isValid() ? previous : next).data(Color).toInt(), Color);
    items.first()->setData(qMax(42, qMax(previous.data(Width).toInt(), next.data(Width).toInt())), Width);
    // With no unique versions, the group supplies the first commit's incoming line.
    if (expanded && row == 0 && next.isValid()) model.item(row, 0)->setData(true, Incoming);
    model.insertRow(row, items);
}
HitResult hitTest(const QRect &rect, const QModelIndex &index, const QPoint &position)
{
    if (!index.isValid() || index.column() != 0 || index.data(AncestorCount).toInt() > 0 || !rect.contains(position))
        return {};
    const int node = index.data(Node).toInt();
    const auto x = [&](int lane) { return rect.left() + 14 + lane * 18; };
    const int middle = rect.center().y();
    const int bottom = rect.bottom() + 1;
    const int nodeColor = index.data(Color).toInt();

    struct Candidate {
        QStringList refs;
        int colorIndex{-1};
        double distance{999.0};
    };
    QVector<Candidate> candidates;

    if (index.data(Incoming).toBool() && position.y() >= rect.top() && position.y() <= middle)
    {
        const double dist = qAbs(position.x() - x(node));
        if (dist <= 8.0)
        {
            const auto nodeRefs = index.data(NodeRefs).toStringList();
            const QStringList refs = nodeRefs.isEmpty() ? QStringList{} : QStringList{nodeRefs.first()};
            candidates.append({refs, nodeColor, dist});
        }
    }

    for (const auto &value : index.data(Edges).toList())
    {
        const auto edge = value.toList();
        if (edge.size() < 4) continue;
        const int from = edge[0].toInt(), to = edge[1].toInt();
        const int colorIdx = edge[2].toInt();
        const auto edgeRefs = edge[3].toStringList();
        const int start = (from == node) ? middle : rect.top();

        if (position.y() >= start && position.y() <= bottom)
        {
            const double t = qBound(0.0, (position.y() - start) / double(qMax(1, bottom - start)), 1.0);
            const double x0 = x(from), x1 = x(to);
            const double curveX = (from == to) ? x0 : (x0 + (x1 - x0) * (3.0 * t * t - 2.0 * t * t * t));
            const double dist = qAbs(position.x() - curveX);
            if (dist <= 8.0)
            {
                const QStringList refs = (from == to && !edgeRefs.isEmpty()) ? QStringList{edgeRefs.first()} : edgeRefs;
                candidates.append({refs, colorIdx, dist});
            }
        }
    }

    const double nodeDist = QLineF(position, QPointF(x(node), middle)).length();
    if (nodeDist <= 9.0)
    {
        const auto tipRefs = index.data(TipRefs).toStringList();
        const auto nodeRefs = index.data(NodeRefs).toStringList();
        const bool isMerge = index.data(Merge).toBool();
        QStringList refs;
        if (tipRefs.size() > 1)
            refs = tipRefs;
        else if (isMerge)
            refs = nodeRefs;
        else if (!nodeRefs.isEmpty())
            refs = QStringList{nodeRefs.first()};
        candidates.append({refs, nodeColor, nodeDist});
    }

    const auto lanes = index.data(LaneRefs).toList();
    const int lane = qRound((position.x() - (rect.left() + 14)) / 18.0);
    if (lane >= 0 && lane < lanes.size())
    {
        const double dist = qAbs(position.x() - x(lane));
        if (dist <= 8.0)
        {
            int laneColor = (lane == node) ? nodeColor : -1;
            for (const auto &value : index.data(Edges).toList())
            {
                const auto edge = value.toList();
                if (edge.size() >= 3 && edge[0].toInt() == lane)
                {
                    laneColor = edge[2].toInt();
                    break;
                }
            }
            const auto laneRefList = lanes[lane].toStringList();
            const QStringList refs = laneRefList.isEmpty() ? QStringList{} : QStringList{laneRefList.first()};
            candidates.append({refs, laneColor >= 0 ? laneColor : nodeColor, dist});
        }
    }

    if (candidates.isEmpty())
        return {};

    auto best = candidates.first();
    for (int i = 1; i < candidates.size(); ++i)
    {
        if (candidates[i].distance < best.distance)
            best = candidates[i];
    }

    return {best.refs.isEmpty() ? QStringList{"HEAD"} : best.refs, best.colorIndex};
}

QStringList hit(const QRect &rect, const QModelIndex &index, const QPoint &position)
{
    return hitTest(rect, index, position).refs;
}

void paint(QPainter *p, const QRect &rect, const QModelIndex &index, const HoverInfo &hover)
{
    const int node = index.data(Node).toInt();
    const auto x = [&](int lane) { return rect.left() + 14 + lane * 18; };
    const int nodeColorIdx = index.data(Color).toInt();
    const auto nodeColor = color(nodeColorIdx);
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

    const auto matchesHover = [&](int colorIdx, const QStringList &refs) -> bool {
        if (hover.progress <= 0.0) return false;
        if (hover.colorIndex >= 0 && colorIdx != hover.colorIndex) return false;
        if (!hover.refs.isEmpty()) {
            for (const auto &ref : refs) {
                if (hover.refs.contains(ref)) return true;
            }
            return false;
        }
        return true;
    };

    if (index.data(Incoming).toBool())
    {
        const auto nodeRefs = index.data(NodeRefs).toStringList();
        const bool hovered = matchesHover(nodeColorIdx, nodeRefs);
        if (hovered && hover.progress > 0.0)
        {
            QColor halo = nodeColor;
            halo.setAlpha(qRound(32 * hover.progress));
            p->setPen(QPen(halo, 4.2 + 0.8 * hover.progress, Qt::SolidLine, Qt::RoundCap));
            p->drawLine(x(node), rect.top(), x(node), middle);
        }
        const double penWidth = hovered ? (1.7 + 0.5 * hover.progress) : 1.7;
        p->setPen(QPen(nodeColor, penWidth, Qt::SolidLine, Qt::RoundCap));
        p->drawLine(x(node), rect.top(), x(node), middle);
    }

    for (const auto &value : index.data(Edges).toList())
    {
        const auto edge = value.toList();
        const int from = edge[0].toInt(), to = edge[1].toInt();
        const int edgeColorIdx = edge[2].toInt();
        const auto edgeRefs = edge.size() >= 4 ? edge[3].toStringList() : QStringList{};
        const auto edgeColor = color(edgeColorIdx);
        const int start = from == node ? middle : rect.top();
        const bool hovered = matchesHover(edgeColorIdx, edgeRefs);

        QPainterPath line(QPointF(x(from), start));
        if (from == to) line.lineTo(x(to), bottom);
        else line.cubicTo(x(from), start + (bottom - start) * .65, x(to), start + (bottom - start) * .35, x(to), bottom);

        if (hovered && hover.progress > 0.0)
        {
            QColor halo = edgeColor;
            halo.setAlpha(qRound(32 * hover.progress));
            p->setPen(QPen(halo, 4.2 + 0.8 * hover.progress, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p->drawPath(line);
        }
        const double penWidth = hovered ? (1.7 + 0.5 * hover.progress) : 1.7;
        p->setPen(QPen(edgeColor, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p->drawPath(line);
    }

    const auto nodeRefs = index.data(NodeRefs).toStringList();
    const bool nodeHovered = matchesHover(nodeColorIdx, nodeRefs);
    const double nodeProgress = nodeHovered ? hover.progress : 0.0;
    const bool ring = index.data(Merge).toBool() || index.data(Current).toBool();

    if (nodeHovered && nodeProgress > 0.0)
    {
        QColor halo = nodeColor;
        halo.setAlpha(qRound(36 * nodeProgress));
        p->setPen(Qt::NoPen);
        p->setBrush(halo);
        const double haloRadius = (ring ? 5.0 : 3.0) + 1.8 * nodeProgress;
        p->drawEllipse(QPointF(x(node), middle), haloRadius, haloRadius);
    }

    const double baseRadius = (ring ? 5.0 : 3.0) + 0.5 * nodeProgress;
    p->setPen(QPen(nodeColor, 1.8 + 0.2 * nodeProgress));
    p->setBrush(ring ? UiStyle::colors().canvas : nodeColor);
    p->drawEllipse(QPointF(x(node), middle), baseRadius, baseRadius);

    if (ring) {
        p->setPen(Qt::NoPen);
        p->setBrush(nodeColor);
        const double innerRadius = 2.0 + 0.2 * nodeProgress;
        p->drawEllipse(QPointF(x(node), middle), innerRadius, innerRadius);
    }

    p->restore();
}
}
