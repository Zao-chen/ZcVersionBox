#include "homepage_page_branches_row.h"
#include "windows/mainwindow_presentation.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace
{
// Text that shortens with an ellipsis instead of widening the row.
class ElidedLabel : public QLabel
{
  public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return {0, QLabel::minimumSizeHint().height()}; }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.setFont(font());
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
    }
};
} // namespace

HomePageBranchRow::HomePageBranchRow(const BranchInfo &branch, const QString &name, QWidget *parent)
    : QFrame(parent), m_branch(branch)
{
    setObjectName("branchRow");
    setProperty("ref", branch.ref);
    setFocusPolicy(Qt::TabFocus); // clicks never leave a row looking selected.
    const auto sharing = branch.remoteBranch ? QStringLiteral("仅在云端，尚未加入") : branch.upstream.isEmpty() ? QStringLiteral("仅在本机") : QStringLiteral("已共享到云端");
    setAccessibleName(name + "，" + (branch.current ? QStringLiteral("正在使用，") : QString()) + sharing);
    setToolTip("点击查看这个方案的历史版本");
    setCursor(Qt::PointingHandCursor);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 8, 8, 8);
    layout->setSpacing(8);
    auto *texts = new QVBoxLayout;
    texts->setSpacing(2);
    auto *titleLine = new QHBoxLayout;
    titleLine->setSpacing(6);
    auto *title = new ElidedLabel(name, this);
    title->setObjectName("branchName");
    UiStyle::text(title, UiStyle::FontRole::Body);
    titleLine->addWidget(title, 1);
    if (branch.remoteBranch || !branch.upstream.isEmpty())
    {
        auto *cloud = new QLabel(this);
        cloud->setObjectName("branchSharing");
        cloud->setPixmap(UiStyle::icon("cloud").pixmap(14, 14));
        cloud->setToolTip(sharing);
        titleLine->addWidget(cloud);
    }
    titleLine->addStretch(0);
    texts->addLayout(titleLine);
    auto detail = branch.message.isEmpty() ? QStringLiteral("（无提交说明）") : branch.message;
    if (branch.committedAt.isValid()) detail += " · " + relativeTime(branch.committedAt);
    auto *caption = new ElidedLabel(detail, this);
    caption->setObjectName("branchDetail");
    UiStyle::text(caption, UiStyle::FontRole::Caption, true);
    texts->addWidget(caption);
    layout->addLayout(texts, 1);

    if (!branch.current)
    {
        m_use = new QPushButton(branch.remoteBranch ? "加入…" : "切换到此方案", this);
        m_use->setObjectName("branchUse");
        auto policy = m_use->sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        m_use->setSizePolicy(policy);
        m_use->installEventFilter(this);
        connect(m_use, &QPushButton::clicked, this, &HomePageBranchRow::useRequested);
        layout->addWidget(m_use);
    }
    m_more = new QToolButton(this);
    m_more->setObjectName("branchMore");
    m_more->setIcon(UiStyle::icon("more"));
    m_more->setAutoRaise(true);
    m_more->setToolTip("更多操作");
    m_more->setAccessibleName("更多操作：" + name);
    m_more->installEventFilter(this);
    connect(m_more, &QToolButton::clicked, this, [this] { emit menuRequested(m_more->mapToGlobal(QPoint(0, m_more->height()))); });
    layout->addWidget(m_more);
    updateHover();
}

void HomePageBranchRow::setActionsEnabled(bool enabled, const QString &reason)
{
    if (!m_use) return;
    m_use->setEnabled(enabled);
    m_use->setToolTip(reason);
}

void HomePageBranchRow::updateHover()
{
    // Focus only highlights a row while navigating by keyboard; there is no selected state.
    const bool focused = hasFocus() || isAncestorOf(QApplication::focusWidget());
    m_hovered = underMouse() || (focused && UiStyle::isKeyboardNavigationActive());
    // Cloud-only 方案 always show how to join; local ones reveal the switch on demand.
    if (m_use) m_use->setVisible(m_branch.remoteBranch || m_hovered);
    update();
}

void HomePageBranchRow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto colors = UiStyle::colors();
    const QRectF row = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
    if (m_hovered)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.hover);
        painter.drawRoundedRect(row, 6, 6);
    }
    if (m_branch.current)
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.added);
        painter.drawRoundedRect(QRectF(row.left() + 2, row.top() + 10, 3, row.height() - 20), 1.5, 1.5);
    }
    if (hasFocus() && UiStyle::isKeyboardNavigationActive())
    {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(colors.secondary);
        painter.drawRoundedRect(row, 6, 6);
    }
}

void HomePageBranchRow::enterEvent(QEnterEvent *event)
{
    QFrame::enterEvent(event);
    updateHover();
}
void HomePageBranchRow::leaveEvent(QEvent *event)
{
    QFrame::leaveEvent(event);
    updateHover();
}
void HomePageBranchRow::focusInEvent(QFocusEvent *event)
{
    QFrame::focusInEvent(event);
    updateHover();
}
void HomePageBranchRow::focusOutEvent(QFocusEvent *event)
{
    QFrame::focusOutEvent(event);
    updateHover();
}
bool HomePageBranchRow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) updateHover();
    return QFrame::eventFilter(watched, event);
}

void HomePageBranchRow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit historyRequested();
    else QFrame::mouseReleaseEvent(event);
}

void HomePageBranchRow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key())
    {
    case Qt::Key_Return:
    case Qt::Key_Enter: emit historyRequested(); return;
    case Qt::Key_Up: emit stepRequested(-1); return;
    case Qt::Key_Down: emit stepRequested(1); return;
    default: QFrame::keyPressEvent(event);
    }
}

void HomePageBranchRow::contextMenuEvent(QContextMenuEvent *event)
{
    emit menuRequested(event->reason() == QContextMenuEvent::Keyboard ? mapToGlobal(rect().bottomLeft()) : event->globalPos());
}
