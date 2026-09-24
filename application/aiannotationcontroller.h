#pragma once
#include "backup/backupservice.h"
#include <QPointer>

class AiAnnotationController : public QObject
{
    Q_OBJECT
  public:
    explicit AiAnnotationController(Backup::BackupService *service, QObject *parent = nullptr);
    ~AiAnnotationController() override;

  private:
    struct Job
    {
        QString id, revision;
        int attempts = 0;
        QPointer<QObject> context;
        bool active = false;
    };
    Backup::BackupService *m_service;
    QMap<QString, Job> m_jobs;
    QMap<QString, QString> m_requests;
    QQueue<QString> m_queue;
    int m_active = 0;
    void enqueue(const QString &id, const QString &revision);
    void dispatch();
    void generate(const QString &key, const QString &diff);
    void complete(const QString &key, const QString &error = {}, bool retryable = false);
    void persist(const Job &job, const QString &state, const QString &error = {});
    void resume();
    bool enabled() const;
};
