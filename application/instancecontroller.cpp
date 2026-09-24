#include "instancecontroller.h"
#include "backup/types.h"
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTimer>

InstanceController::InstanceController(const QString &root, QObject *parent)
    : QObject(parent), m_name("ZcVersionBox-" + QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex().left(24))), m_lock(QDir(root).filePath("instance.lock"))
{
    if (!QDir().mkpath(root))
        throw Backup::Error(Backup::ErrorCode::Permission, "无法创建应用数据目录");
    m_lock.setStaleLockTime(0);
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this]
            {
                while (auto socket = m_server.nextPendingConnection())
                {
                    socket->setParent(this);
                    socket->setReadBufferSize(1024 * 1024 + 1);
                    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
                    QTimer::singleShot(5000, socket, [socket]
                                       {
                                           socket->disconnectFromServer();
                                       });
                    connect(socket, &QLocalSocket::readyRead, this, [this, socket]
                            {
                                if (socket->bytesAvailable() > 1024 * 1024)
                                {
                                    socket->abort();
                                    return;
                                }
                                if (!socket->canReadLine())
                                    return;
                                const auto value = QJsonDocument::fromJson(socket->readLine()).object();
                                if (value["version"].toInt() != 1 || !value["paths"].isArray())
                                {
                                    socket->write("error\n");
                                    socket->disconnectFromServer();
                                    return;
                                }
                                QStringList paths;
                                for (const auto &path : value["paths"].toArray())
                                {
                                    if (!path.isString() || !QDir::isAbsolutePath(path.toString()))
                                    {
                                        socket->write("error\n");
                                        socket->disconnectFromServer();
                                        return;
                                    }
                                    paths.append(path.toString());
                                }
                                if (paths.isEmpty())
                                    emit activateRequested();
                                else
                                    emit pathsReceived(paths);
                                socket->write("accepted\n");
                                socket->disconnectFromServer();
                            });
                }
            });
}
bool InstanceController::acquire()
{
    if (!m_lock.tryLock(0))
        return false;
    QLocalServer::removeServer(m_name);
    if (!m_server.listen(m_name))
        throw Backup::Error(Backup::ErrorCode::Io, "无法创建本地任务服务：" + m_server.errorString());
    return true;
}
void InstanceController::stopAccepting()
{
    m_server.close();
    for (auto socket : findChildren<QLocalSocket *>())
        socket->abort();
}
bool InstanceController::forward(const QStringList &paths, QString *error)
{
    QLocalSocket socket;
    socket.connectToServer(m_name);
    if (!socket.waitForConnected(5000))
    {
        *error = "无法连接已运行的实例：" + socket.errorString();
        return false;
    }
    const auto message = QJsonDocument(QJsonObject{{"version", 1}, {"paths", QJsonArray::fromStringList(paths)}}).toJson(QJsonDocument::Compact) + '\n';
    socket.write(message);
    socket.waitForBytesWritten(5000);
    if (!socket.canReadLine())
        socket.waitForReadyRead(5000);
    if (socket.readLine() != "accepted\n")
    {
        *error = "已运行的实例未接收请求";
        return false;
    }
    return true;
}
