#include "utils/update_service.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

class UpdateServiceTest : public QObject
{
    Q_OBJECT

  private slots:
    void comparesNumericVersions();
    void parsesStableRelease();
    void rejectsInvalidRelease();
};

void UpdateServiceTest::comparesNumericVersions()
{
    QCOMPARE(UpdateService::compareVersions("2.0.0", "2.0.0"), 0);
    QCOMPARE(UpdateService::compareVersions("v2.1.0", "2.0.9"), 1);
    QCOMPARE(UpdateService::compareVersions("1.12.0", "1.9.9"), 1);
    QCOMPARE(UpdateService::compareVersions("1.2.3", "1.2.4"), -1);
    QCOMPARE(UpdateService::compareVersions("nightly", "1.0.0"), 0);
}

void UpdateServiceTest::parsesStableRelease()
{
    QJsonObject asset;
#if defined(Q_OS_WIN)
    asset.insert("name", "ZcVersionBox-v2.1.0-setup.exe");
#elif defined(Q_OS_MACOS)
    asset.insert("name", "ZcVersionBox-v2.1.0-mac-arm64_x86_64.dmg");
#elif defined(Q_OS_LINUX)
    asset.insert("name", "ZcVersionBox-v2.1.0-linux-amd64.deb");
#else
    asset.insert("name", "ZcVersionBox-v2.1.0-unknown.bin");
#endif
    asset.insert("browser_download_url", "https://github.com/Zao-chen/ZcVersionBox/releases/download/v2.1.0/package");
    asset.insert("size", 1234);

    QJsonObject release;
    release.insert("tag_name", "v2.1.0");
    release.insert("name", "ZcVersionBox v2.1.0");
    release.insert("body", "修复更新体验");
    release.insert("html_url", "https://github.com/Zao-chen/ZcVersionBox/releases/tag/v2.1.0");
    release.insert("published_at", "2026-10-03T12:00:00Z");
    release.insert("assets", QJsonArray{asset});

    QString error;
    const auto parsed = UpdateService::parseRelease(QJsonDocument(release).toJson(), "2.0.0", &error);
    QVERIFY2(parsed.has_value(), qPrintable(error));
    QCOMPARE(parsed->version, "2.1.0");
    QCOMPARE(parsed->tagName, "v2.1.0");
    QCOMPARE(parsed->notes, "修复更新体验");
    QCOMPARE(parsed->downloadSize, qint64(1234));
#if defined(Q_OS_WIN)
    QCOMPARE(parsed->downloadName, "ZcVersionBox-v2.1.0-setup.exe");
#elif defined(Q_OS_MACOS)
    QCOMPARE(parsed->downloadName, "ZcVersionBox-v2.1.0-mac-arm64_x86_64.dmg");
#elif defined(Q_OS_LINUX)
    QCOMPARE(parsed->downloadName, "ZcVersionBox-v2.1.0-linux-amd64.deb");
#else
    QVERIFY(parsed->downloadName.isEmpty());
#endif
}

void UpdateServiceTest::rejectsInvalidRelease()
{
    QString error;
    QVERIFY(!UpdateService::parseRelease("{\"tag_name\":\"latest\"}", "2.0.0", &error));
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(!UpdateService::parseRelease("{\"tag_name\":\"v2.1.0\",\"prerelease\":true}", "2.0.0", &error));
    QVERIFY(!error.isEmpty());
}

QTEST_APPLESS_MAIN(UpdateServiceTest)

#include "update_service.moc"
