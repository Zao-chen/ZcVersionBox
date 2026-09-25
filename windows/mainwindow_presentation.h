#pragma once
#include "utils/operationresult.h"
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QAction;
class QAbstractItemView;
class QLabel;
class QPushButton;
class QToolButton;
namespace oclero::qlementine
{
class QlementineStyle;
class Popover;
} // namespace oclero::qlementine

// Presentation values and small helpers, not a second widget framework.
namespace UiStyle
{
enum class FontRole
{
    Page,
    Object,
    Section,
    Body,
    Sidebar,
    Caption,
    Code
};
enum class Surface
{
    Canvas,
    Sidebar,
    Popup
};
struct Colors
{
    QColor canvas, sidebar, popup, text, secondary, hover, selected, separator, added, removed;
};
Colors colors();
QFont font(FontRole role);
QIcon icon(const QString &name);
void text(QWidget *widget, FontRole role = FontRole::Body, bool secondary = false);
void surface(QWidget *widget, Surface role);
void flatView(QAbstractItemView *view);
QAction *action(QObject *owner, const QString &name, const QString &label, const QString &iconName = {});
QToolButton *toolButton(QWidget *parent, QAction *action, bool iconOnly = false);
int rowHeight(int minimum, const QFont &font, bool twoLines = false);
bool isKeyboardNavigationActive();
void setKeyboardNavigationActive(bool active);
} // namespace UiStyle

class ThemeController : public QObject
{
    Q_OBJECT
  public:
    explicit ThemeController(QObject *parent = nullptr);
    void toggle();
    bool isDark() const { return m_dark; }
    QIcon icon(const QString &name) const { return UiStyle::icon(name); }
  signals:
    void changed();

  private:
    bool m_dark{false};
    oclero::qlementine::QlementineStyle *m_style;
    void apply();
};

// An overlay owned by the window. It never participates in a page layout.
class NotificationBar : public QWidget
{
    Q_OBJECT
  public:
    explicit NotificationBar(QWidget *parent);
    void showResult(const OperationResult &result);
    void dismiss();

  protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

  private:
    QLabel *m_symbol;
    QLabel *m_title;
    QLabel *m_text;
    QPushButton *m_detailsButton;
    QToolButton *m_close;
    QPointer<oclero::qlementine::Popover> m_popover;
    QTimer m_timer;
    QString m_details;
    int m_remaining{0};
    quint64 m_serial{0};
    NotificationLevel m_level{NotificationLevel::Information};
    void positionOverlay();
    void openDetails();
    void closeDetails();
    void updateColors();
};

QString formatBytes(qint64 bytes);
bool confirmAction(QWidget *owner, const QString &text, const QString &action);
void openLocalPath(QWidget *owner, const QString &path);
