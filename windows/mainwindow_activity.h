#pragma once
#include <QTimer>
#include <QWidget>

class QLabel;
namespace oclero::qlementine { class LoadingSpinner; }

// The one progress affordance for work the user waits on: a spinner with an
// optional caption. It appears after a short delay so quick work never flashes.
class ActivityIndicator : public QWidget
{
    Q_OBJECT
  public:
    explicit ActivityIndicator(QWidget *parent, int spinnerSize = 16);
    void setActivity(bool active, const QString &label = {});
    void setCaptionVisible(bool visible);
    bool isActive() const { return m_active; }

  private:
    oclero::qlementine::LoadingSpinner *m_spinner;
    QLabel *m_caption;
    QTimer m_delay;
    bool m_active{false};
    bool m_captionVisible{true};
};
