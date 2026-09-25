#pragma once
#include "utils/settingsservice.h"
#include "windows/mainwindow_navigation.h"
#include <QWidget>
#include <memory>
namespace Ui
{
class SettingPage;
class SettingPageAiPage;
} // namespace Ui
namespace oclero::qlementine
{
class Switch;
class LoadingSpinner;
} // namespace oclero::qlementine
class ThemeController;
class SettingPage : public QWidget
{
    Q_OBJECT
  public:
    SettingPage(SettingsService *settings, ThemeController *theme, QWidget *parent = nullptr);
    ~SettingPage();
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    std::unique_ptr<Ui::SettingPage> ui;
    ThemeController *m_theme;
};
class SettingPageAiPage : public QWidget
{
    Q_OBJECT
  public:
    SettingPageAiPage(SettingsService *settings, QWidget *parent = nullptr);
    ~SettingPageAiPage();
    void refresh();
  signals:
    void navigate(const Route &route);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    std::unique_ptr<Ui::SettingPageAiPage> ui;
    SettingsService *m_settings;
    oclero::qlementine::Switch *m_enabled;
    oclero::qlementine::LoadingSpinner *m_spinner;
    bool m_loading{false};
    QString m_displayedProvider;
    QString m_fetchError;
    void updateLoadingState();
};
