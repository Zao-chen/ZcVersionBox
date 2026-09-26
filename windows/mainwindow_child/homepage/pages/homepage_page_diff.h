#pragma once
#include "utils/backupservice.h"
#include "utils/settingsservice.h"
#include "windows/mainwindow_navigation.h"
#include <QStandardItemModel>
#include <QWidget>
#include <memory>
namespace Ui
{
class HomePageDiffPage;
}
namespace oclero::qlementine
{
class Expander;
class LoadingSpinner;
} // namespace oclero::qlementine
class QAction;
class QSyntaxHighlighter;
class HomePageDiffPage : public QWidget
{
    Q_OBJECT
  public:
    HomePageDiffPage(BackupService *service, SettingsService *settings, AiGateway *gateway, QWidget *parent = nullptr);
    ~HomePageDiffPage();
    void setRevision(const QString &id, const QString &commit);
    void deactivate();
    void refreshTheme();
    QList<QAction *> toolbarActions() const;
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);
    void titleChanged(const QString &title);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    struct ViewState
    {
        quint64 generation{0};
        QString file, analysis;
        int fileScroll{0};
        bool expanded{false};
        QHash<QString, QPoint> scrolls;
    };
    std::unique_ptr<Ui::HomePageDiffPage> ui;
    BackupService *m_service;
    SettingsService *m_settings;
    AiGateway *m_gateway;
    QStandardItemModel m_model;
    QAction *m_analyze;
    QSyntaxHighlighter *m_highlighter;
    oclero::qlementine::Expander *m_expander;
    oclero::qlementine::LoadingSpinner *m_spinner;
    QHash<QString, ViewState> m_states;
    QHash<QString, QPoint> m_fileScrolls;
    QString m_id, m_currentFile, m_analysisStatus;
    DiffData m_diff;
    quint64 m_generation{0}, m_repositoryGeneration{0}, m_fileGeneration{0};
    bool m_active{false}, m_valid{false}, m_loading{false}, m_hasAnalysis{false};
    void rememberState();
    void loadFile();
    void analyze();
    void updateLoadingState();
    void updateResponsiveLayout();
};
