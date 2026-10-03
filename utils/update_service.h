#pragma once

#include <QDateTime>
#include <QObject>
#include <QUrl>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;
class QTimer;

struct UpdateRelease
{
    QString currentVersion;
    QString version;
    QString tagName;
    QString name;
    QString notes;
    QDateTime publishedAt;
    QUrl releaseUrl;
    QUrl downloadUrl;
    QString downloadName;
    qint64 downloadSize{0};
};

struct UpdateCheckResult
{
    bool success{false};
    bool updateAvailable{false};
    UpdateRelease release;
    QString error;
};

Q_DECLARE_METATYPE(UpdateRelease)
Q_DECLARE_METATYPE(UpdateCheckResult)

class UpdateService : public QObject
{
    Q_OBJECT
  public:
    explicit UpdateService(QObject *parent = nullptr);
    ~UpdateService() override;

    bool isChecking() const { return m_checkReply != nullptr; }
    bool isDownloading() const { return m_downloadReply != nullptr; }

    void checkForUpdates(const QString &currentVersion = {});
    void downloadUpdate(const UpdateRelease &release);
    void cancelDownload();

    static int compareVersions(const QString &left, const QString &right);
    static std::optional<UpdateRelease> parseRelease(const QByteArray &payload,
                                                     const QString &currentVersion,
                                                     QString *error = nullptr);
    static QUrl releaseApiUrl();

  signals:
    void checkingChanged(bool checking);
    void checkFinished(const UpdateCheckResult &result);
    void downloadingChanged(bool downloading);
    void downloadProgress(qint64 received, qint64 total);
    void downloadFinished(const QString &path);
    void downloadFailed(const QString &error);

  private:
    QNetworkAccessManager *m_network;
    QNetworkReply *m_checkReply{nullptr};
    QNetworkReply *m_downloadReply{nullptr};
    std::unique_ptr<QSaveFile> m_downloadFile;
    QString m_currentVersion;
    QString m_downloadPath;
    bool m_checkTimedOut{false};
    bool m_downloadTimedOut{false};
    bool m_downloadWriteFailed{false};
    QTimer *m_checkTimeout;
    QTimer *m_downloadTimeout;

    void finishCheck(QNetworkReply *reply);
    void finishDownload(QNetworkReply *reply);
    static QString errorForReply(QNetworkReply *reply, bool timedOut);
    static QString downloadPath(const QString &fileName);
    static bool isSecureUrl(const QUrl &url);
    static QString platformAssetSuffix();
};
