#pragma once
#include "utils/update_service.h"
#include <QWidget>
#include <memory>
#include <optional>
namespace Ui
{
class AboutPage;
}
class AboutPage : public QWidget
{
    Q_OBJECT
  public:
    explicit AboutPage(UpdateService *updates, QWidget *parent = nullptr);
    ~AboutPage();
    void setAvailableUpdate(const UpdateRelease &release);
    void showAvailableUpdate();

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    std::unique_ptr<Ui::AboutPage> ui;
    UpdateService *m_updates;
    bool m_manualCheckPending{false};
    std::optional<UpdateRelease> m_availableUpdate;
    void showRelease(const UpdateRelease &release);
};
