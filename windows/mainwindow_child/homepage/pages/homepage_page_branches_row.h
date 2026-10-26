#pragma once

#include "utils/backup_types.h"
#include <QFrame>

class QLabel;
class QPushButton;
class QToolButton;

// One 方案 in the list. Rows carry their own actions; there is no selection state.
class HomePageBranchRow : public QFrame
{
    Q_OBJECT
  public:
    HomePageBranchRow(const BranchInfo &branch, const QString &name, QWidget *parent = nullptr);
    const BranchInfo &branch() const { return m_branch; }
    void setActionsEnabled(bool enabled, const QString &reason = {});
  signals:
    void useRequested();
    void historyRequested();
    void menuRequested(const QPoint &globalPos);
    void stepRequested(int delta);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    BranchInfo m_branch;
    QPushButton *m_use{nullptr};
    QToolButton *m_more{nullptr};
    bool m_hovered{false};
    void updateHover();
};
