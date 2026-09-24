#include "application/instancecontroller.h"
#include <QCoreApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class InstanceTests : public QObject
{
    Q_OBJECT
  private slots:
    void forwardToOnlyWriter()
    {
        QTemporaryDir root;
        InstanceController owner(root.path());
        QVERIFY(owner.acquire());
        InstanceController duplicate(root.path());
        QVERIFY(!duplicate.acquire());
        QSignalSpy paths(&owner, &InstanceController::pathsReceived);
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--forward", root.path(), root.path() + "/中文 path"});
        QVERIFY(child.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(paths.size(), 1, 7000);
        QTRY_COMPARE_WITH_TIMEOUT(child.state(), QProcess::NotRunning, 7000);
        QCOMPARE(child.exitCode(), 0);
        QCOMPARE(paths.first().first().toStringList(), QStringList{root.path() + "/中文 path"});
    }
    void activateAndSeparateProfiles()
    {
        QTemporaryDir root, other;
        InstanceController first(root.path()), second(other.path());
        QVERIFY(first.acquire());
        QVERIFY(second.acquire());
        QSignalSpy activation(&first, &InstanceController::activateRequested);
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--forward", root.path()});
        QVERIFY(child.waitForStarted());
        QTRY_COMPARE_WITH_TIMEOUT(activation.size(), 1, 7000);
        QTRY_COMPARE(child.state(), QProcess::NotRunning);
        QCOMPARE(child.exitCode(), 0);
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    if (arguments.size() >= 3 && arguments[1] == "--forward")
    {
        InstanceController client(arguments[2]);
        QString error;
        return client.forward(arguments.mid(3), &error) ? 0 : 1;
    }
    InstanceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "instance_tests.moc"
