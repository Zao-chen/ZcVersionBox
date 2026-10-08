#pragma once
#include "utils/backupservice.h"
#include "utils/diff_parser.h"
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
    ~HomePageDiffPage() override;
    void setRevision(const QString &id, const QString &commit, const QString &oldCommit = {});
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
    oclero::qlementine::LoadingSpinner *m_spinner;
    oclero::qlementine::Expander *m_expander{nullptr};

    QHash<QString, ViewState> m_states;
    QHash<QString, QPoint> m_fileScrolls;
    QString m_id, m_currentFile, m_analysisStatus, m_analysisText;
    DiffData m_diff;
    DiffParser::ParsedDiff m_currentParsedDiff;
    QString m_currentRawDiff;
    quint64 m_generation{0}, m_repositoryGeneration{0}, m_fileGeneration{0};
    bool m_active{false}, m_valid{false}, m_loading{false}, m_hasAnalysis{false};

    void rememberState();
    void loadFile();
    void analyze();
    void updateLoadingState();
    void updateResponsiveLayout();
    void copyCurrentPath();
};
