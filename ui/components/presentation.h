#pragma once
#include "services/operationresult.h"
#include "ui/navigation.h"
#include <QIcon>
#include <QTimer>
#include <QWidget>

class QLabel;
class QHBoxLayout;
namespace oclero::qlementine
{
class QlementineStyle;
}

class ThemeController : public QObject
{
    Q_OBJECT
  public:
    explicit ThemeController(QObject *parent = nullptr);
    void toggle();
    bool isDark() const { return m_dark; }
    QIcon icon(const QString &name) const;
  signals:
    void changed();

  private:
    bool m_dark{false};
    oclero::qlementine::QlementineStyle *m_style;
    void apply();
};

class NotificationBar : public QWidget
{
    Q_OBJECT
  public:
    explicit NotificationBar(QWidget *parent = nullptr);
    void showResult(const OperationResult &result);

  private:
    QLabel *m_text;
    QTimer m_timer;
    NotificationLevel m_level{NotificationLevel::Information};

  protected:
    void paintEvent(QPaintEvent *event) override;
};
class BreadcrumbBar : public QWidget
{
    Q_OBJECT
  public:
    explicit BreadcrumbBar(QWidget *parent = nullptr);
    void setRoute(const Route &route, const QString &name);
  signals:
    void navigate(const Route &route);

  private:
    QHBoxLayout *m_layout;
};

QString formatBytes(qint64 bytes);
bool confirmAction(QWidget *owner, const QString &text, const QString &action);
void openLocalPath(QWidget *owner, const QString &path);
