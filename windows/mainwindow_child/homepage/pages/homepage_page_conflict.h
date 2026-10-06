#pragma once
#include "utils/backupservice.h"
#include "windows/mainwindow_navigation.h"
#include <QStandardItemModel>
#include <QWidget>
#include <memory>

namespace Ui { class HomePageConflictPage; }
class QPlainTextEdit;
class HomePageConflictPage : public QWidget
{
    Q_OBJECT
  public:
    explicit HomePageConflictPage(BackupService *service, QWidget *parent = nullptr);
    ~HomePageConflictPage() override;
    void setBackup(const QString &id, const QString &returnBranchRef = {});
    void deactivate();
    void refreshTheme();
  signals:
    void navigate(const Route &route);
    void notification(const OperationResult &result);
  protected:
    void resizeEvent(QResizeEvent *event) override;
  private:
    struct Context { QString id; quint64 repository, page; };
    struct Position { QString session, path; int hunk{0}; bool preview{false}; };
    std::unique_ptr<Ui::HomePageConflictPage> ui;
    BackupService *m_service;
    QStandardItemModel m_files;
    SyncResolutionSession m_session;
    PreparedSyncApply m_prepared;
    QVector<DiffFile> m_previewFiles;
    QHash<QString, Position> m_positions;
    QString m_id;
    QString m_returnBranchRef;
    quint64 m_repository{0}, m_context{0}, m_contentRequest{0};
    int m_file{0}, m_hunk{0};
    bool m_active{false}, m_busy{false}, m_preview{false};
    QString m_currentLocalText, m_currentRemoteText;
    bool m_hasLocalText{false}, m_hasRemoteText{false};
    Context context() const;
    bool current(const Context &context) const;
    void rememberPosition();
    void load(bool restart = false, bool resumePreview = true);
    void acceptSession(const SyncResolutionSession &session, bool restorePosition, bool resumePreview = true);
    void populateFiles();
    void selectFile();
    void showQuestion();
    void showResult();
    void choose(ConflictChoice choice);
    void moveQuestion(bool next);
    void preparePreview();
    void apply();
    void loadContent(QPlainTextEdit *editor, const QString &path, ConflictSide side, quint64 request);
    void updateActions();
    void updateLayout();
    Route returnRoute() const;
};
