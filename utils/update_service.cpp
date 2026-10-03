#include "update_service.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QRegularExpression>

namespace
{
constexpr auto Repository = "Zao-chen/ZcVersionBox";
constexpr auto UserAgent = "ZcVersionBox-update-checker";
constexpr int RequestTimeoutMs = 12000;

struct ParsedVersion
{
    int major;
    int minor;
    int patch;
};

std::optional<ParsedVersion> parseVersion(const QString &version)
{
    static const QRegularExpression pattern(R"(^v?([0-9]+)\.([0-9]+)\.([0-9]+)$)");
    const auto match = pattern.match(version.trimmed());
    if (!match.hasMatch())
        return std::nullopt;

    bool ok = false;
    const auto major = match.captured(1).toInt(&ok);
    if (!ok)
        return std::nullopt;
    const auto minor = match.captured(2).toInt(&ok);
    if (!ok)
        return std::nullopt;
    const auto patch = match.captured(3).toInt(&ok);
    if (!ok)
        return std::nullopt;
    return ParsedVersion{major, minor, patch};
}

QString normalizedVersion(const QString &version)
{
    return version.trimmed().startsWith('v') ? version.trimmed().mid(1) : version.trimmed();
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}
}

UpdateService::UpdateService(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_checkTimeout(new QTimer(this)),
      m_downloadTimeout(new QTimer(this))
{
    m_checkTimeout->setSingleShot(true);
    m_downloadTimeout->setSingleShot(true);
    connect(m_checkTimeout, &QTimer::timeout, this, [this]
            {
        if (!m_checkReply)
            return;
        m_checkTimedOut = true;
        m_checkReply->abort(); });
    connect(m_downloadTimeout, &QTimer::timeout, this, [this]
            {
        if (!m_downloadReply)
            return;
        m_downloadTimedOut = true;
        m_downloadReply->abort(); });
}

UpdateService::~UpdateService()
{
    cancelDownload();
    if (m_checkReply)
        m_checkReply->abort();
}

QUrl UpdateService::releaseApiUrl()
{
    return QUrl(QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(QString::fromLatin1(Repository)));
}

int UpdateService::compareVersions(const QString &left, const QString &right)
{
    const auto parsedLeft = parseVersion(left);
    const auto parsedRight = parseVersion(right);
    if (!parsedLeft || !parsedRight)
        return 0;
    if (parsedLeft->major != parsedRight->major)
        return parsedLeft->major < parsedRight->major ? -1 : 1;
    if (parsedLeft->minor != parsedRight->minor)
        return parsedLeft->minor < parsedRight->minor ? -1 : 1;
    if (parsedLeft->patch != parsedRight->patch)
        return parsedLeft->patch < parsedRight->patch ? -1 : 1;
    return 0;
}

QString UpdateService::platformAssetSuffix()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("-setup.exe");
#elif defined(Q_OS_MACOS)
    return QStringLiteral(".dmg");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("-linux-amd64.deb");
#else
    return {};
#endif
}

std::optional<UpdateRelease> UpdateService::parseRelease(const QByteArray &payload,
                                                          const QString &currentVersion,
                                                          QString *error)
{
    const auto current = parseVersion(currentVersion);
    if (!current)
    {
        setError(error, QStringLiteral("当前版本号格式无效"));
        return std::nullopt;
    }

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        setError(error, QStringLiteral("更新信息不是有效的 JSON"));
        return std::nullopt;
    }

    const auto object = document.object();
    const auto tagName = object.value(QStringLiteral("tag_name")).toString().trimmed();
    const auto remote = parseVersion(tagName);
    if (!remote)
    {
        setError(error, QStringLiteral("发布版本号格式无效"));
        return std::nullopt;
    }
    if (object.value(QStringLiteral("draft")).toBool() || object.value(QStringLiteral("prerelease")).toBool())
    {
        setError(error, QStringLiteral("最新发布不是稳定版本"));
        return std::nullopt;
    }

    UpdateRelease release;
    release.currentVersion = normalizedVersion(currentVersion);
    release.version = normalizedVersion(tagName);
    release.tagName = tagName;
    release.name = object.value(QStringLiteral("name")).toString().trimmed();
    if (release.name.isEmpty())
        release.name = QStringLiteral("ZcVersionBox %1").arg(tagName);
    release.notes = object.value(QStringLiteral("body")).toString().trimmed();
    release.releaseUrl = QUrl(object.value(QStringLiteral("html_url")).toString());
    if (!isSecureUrl(release.releaseUrl))
        release.releaseUrl = QUrl(QStringLiteral("https://github.com/%1/releases/tag/%2")
                                      .arg(QString::fromLatin1(Repository), tagName));
    release.publishedAt = QDateTime::fromString(object.value(QStringLiteral("published_at")).toString(), Qt::ISODate);

    const auto expectedSuffix = platformAssetSuffix();
    if (!expectedSuffix.isEmpty())
    {
        const auto assets = object.value(QStringLiteral("assets")).toArray();
        for (const auto &assetValue : assets)
        {
            const auto asset = assetValue.toObject();
            const auto name = asset.value(QStringLiteral("name")).toString();
            const auto url = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
            if (!name.endsWith(expectedSuffix, Qt::CaseInsensitive) || !isSecureUrl(url))
                continue;
            release.downloadName = name;
            release.downloadUrl = url;
            release.downloadSize = asset.value(QStringLiteral("size")).toInteger();
            break;
        }
    }
    return release;
}

void UpdateService::checkForUpdates(const QString &currentVersion)
{
    if (m_checkReply)
        return;

    m_currentVersion = currentVersion.trimmed();
    if (m_currentVersion.isEmpty())
        m_currentVersion = QCoreApplication::applicationVersion();
    if (!parseVersion(m_currentVersion))
    {
        UpdateCheckResult result;
        result.error = QStringLiteral("当前版本号格式无效");
        emit checkFinished(result);
        return;
    }

    QNetworkRequest request(releaseApiUrl());
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("%1/%2").arg(QString::fromLatin1(UserAgent), m_currentVersion));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_checkTimedOut = false;
    m_checkReply = m_network->get(request);
    emit checkingChanged(true);
    m_checkTimeout->start(RequestTimeoutMs);
    connect(m_checkReply, &QNetworkReply::finished, this, [this]
            {
        if (m_checkReply)
            finishCheck(m_checkReply); });
}

void UpdateService::finishCheck(QNetworkReply *reply)
{
    m_checkTimeout->stop();
    const auto payload = reply->readAll();
    const auto timedOut = m_checkTimedOut;
    const auto networkError = reply->error();
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_checkReply = nullptr;

    UpdateCheckResult result;
    if (timedOut || networkError != QNetworkReply::NoError)
    {
        result.error = errorForReply(reply, timedOut);
    }
    else if (status < 200 || status >= 300)
    {
        result.error = QStringLiteral("更新服务返回 HTTP %1").arg(status);
    }
    else
    {
        QString parseError;
        const auto parsed = parseRelease(payload, m_currentVersion, &parseError);
        if (!parsed)
            result.error = parseError;
        else
        {
            result.success = true;
            result.release = *parsed;
            result.updateAvailable = compareVersions(result.release.version, result.release.currentVersion) > 0;
        }
    }
    reply->deleteLater();
    emit checkingChanged(false);
    emit checkFinished(result);
}

void UpdateService::downloadUpdate(const UpdateRelease &release)
{
    if (m_downloadReply)
        return;
    if (!isSecureUrl(release.downloadUrl) || release.downloadName.isEmpty())
    {
        emit downloadFailed(QStringLiteral("当前平台没有可用的安装包"));
        return;
    }

    m_downloadPath = downloadPath(release.downloadName);
    if (m_downloadPath.isEmpty())
    {
        emit downloadFailed(QStringLiteral("无法访问系统下载目录"));
        return;
    }
    m_downloadFile = std::make_unique<QSaveFile>(m_downloadPath);
    if (!m_downloadFile->open(QIODevice::WriteOnly))
    {
        m_downloadFile.reset();
        emit downloadFailed(QStringLiteral("无法创建下载文件"));
        return;
    }

    QNetworkRequest request(release.downloadUrl);
    request.setRawHeader("Accept", "application/octet-stream");
    request.setRawHeader("User-Agent", UserAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    m_downloadTimedOut = false;
    m_downloadWriteFailed = false;
    m_downloadReply = m_network->get(request);
    emit downloadingChanged(true);
    m_downloadTimeout->start(10 * 60 * 1000);
    connect(m_downloadReply, &QNetworkReply::readyRead, this, [this]
            {
        if (!m_downloadReply || !m_downloadFile)
            return;
        const auto chunk = m_downloadReply->readAll();
        if (m_downloadFile->write(chunk) != chunk.size())
            m_downloadWriteFailed = true; });
    connect(m_downloadReply, &QNetworkReply::downloadProgress, this, &UpdateService::downloadProgress);
    connect(m_downloadReply, &QNetworkReply::finished, this, [this]
            {
        if (m_downloadReply)
            finishDownload(m_downloadReply); });
}

void UpdateService::finishDownload(QNetworkReply *reply)
{
    m_downloadTimeout->stop();
    if (m_downloadFile && !m_downloadWriteFailed)
    {
        const auto chunk = reply->readAll();
        if (m_downloadFile->write(chunk) != chunk.size())
            m_downloadWriteFailed = true;
    }

    const auto error = reply->error();
    const auto timedOut = m_downloadTimedOut;
    const auto path = m_downloadPath;
    const auto writeFailed = m_downloadWriteFailed;
    m_downloadReply = nullptr;
    QString failure;
    if (timedOut || error != QNetworkReply::NoError)
        failure = errorForReply(reply, timedOut);
    else if (writeFailed)
        failure = QStringLiteral("写入下载文件失败");
    else if (!m_downloadFile || !m_downloadFile->commit())
        failure = QStringLiteral("保存安装包失败");

    m_downloadFile.reset();
    m_downloadPath.clear();
    reply->deleteLater();
    emit downloadingChanged(false);
    if (failure.isEmpty())
        emit downloadFinished(path);
    else
        emit downloadFailed(failure);
}

void UpdateService::cancelDownload()
{
    if (!m_downloadReply)
        return;
    m_downloadReply->abort();
}

QString UpdateService::errorForReply(QNetworkReply *reply, bool timedOut)
{
    if (timedOut)
        return QStringLiteral("连接更新服务超时，请检查网络后重试");
    if (reply->error() == QNetworkReply::OperationCanceledError)
        return QStringLiteral("下载已取消");
    if (!reply->errorString().isEmpty())
        return reply->errorString();
    return QStringLiteral("无法连接更新服务");
}

QString UpdateService::downloadPath(const QString &fileName)
{
    auto directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (directory.isEmpty())
        return {};
    if (!QDir().mkpath(directory))
        return {};

    const auto safeFileName = QFileInfo(fileName).fileName();
    if (safeFileName.isEmpty() || safeFileName == "." || safeFileName == "..")
        return {};
    const auto info = QFileInfo(safeFileName);
    const auto baseName = info.completeBaseName().isEmpty() ? QStringLiteral("ZcVersionBox-update") : info.completeBaseName();
    const auto suffix = info.completeSuffix().isEmpty() ? QString() : QStringLiteral(".") + info.completeSuffix();
    auto candidate = QDir(directory).filePath(safeFileName);
    for (int index = 1; QFileInfo::exists(candidate); ++index)
        candidate = QDir(directory).filePath(QStringLiteral("%1 (%2)%3").arg(baseName).arg(index).arg(suffix));
    return candidate;
}

bool UpdateService::isSecureUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0)
        return false;
    const auto host = url.host().toLower();
    return host == QStringLiteral("github.com") || host.endsWith(QStringLiteral(".github.com")) ||
           host.endsWith(QStringLiteral(".githubusercontent.com"));
}
