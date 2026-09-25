#pragma once
#include "services/settingsservice.h"
#include "ui/navigation.h"
#include <QWidget>
#include <memory>
namespace Ui
{
class GeneralSettingsPage;
class AiSettingsPage;
} // namespace Ui
namespace oclero::qlementine
{
class Switch;
}
class GeneralSettingsPage : public QWidget
{
    Q_OBJECT
  public:
    GeneralSettingsPage(SettingsService *settings, QWidget *parent = nullptr);
    ~GeneralSettingsPage();
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);

  private:
    std::unique_ptr<Ui::GeneralSettingsPage> ui;
};
class AiSettingsPage : public QWidget
{
    Q_OBJECT
  public:
    AiSettingsPage(SettingsService *settings, QWidget *parent = nullptr);
    ~AiSettingsPage();
    void refresh();
  signals:
    void navigate(const Route &route);

  private:
    std::unique_ptr<Ui::AiSettingsPage> ui;
    SettingsService *m_settings;
    oclero::qlementine::Switch *m_enabled;
    bool m_loading{false};
    QString m_displayedProvider;
};
