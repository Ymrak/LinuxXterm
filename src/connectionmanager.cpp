#include "connectionmanager.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>
#include <QSaveFile>

ConnectionManager::ConnectionManager(QObject *parent)
    : QObject(parent)
{
    loadConnections();
}

ConnectionManager::~ConnectionManager()
{
}

QString ConnectionManager::getConfigPath() const
{
    return QDir::homePath() + "/.config/linuxxterm";
}

QString ConnectionManager::getConnectionsFilePath() const
{
    return getConfigPath() + "/connections.json";
}

void ConnectionManager::ensureConfigDirExists()
{
    QDir dir(getConfigPath());
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    QFile::setPermissions(getConfigPath(), QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                               | QFileDevice::ExeOwner);
}

void ConnectionManager::loadConnections()
{
    QString filePath = getConnectionsFilePath();
    QFile file(filePath);

    if (!file.exists()) {
        qDebug() << "Файл подключений не найден, создаём новый список";
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Не удалось открыть файл подключений:" << filePath;
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray()) {
        qWarning() << "Неверный формат файла подключений";
        return;
    }

    QJsonArray array = doc.array();
    m_connections.clear();

    for (const QJsonValue &value : array) {
        if (value.isObject()) {
            Connection conn = Connection::fromJson(value.toObject());
            m_connections.append(conn);
        }
    }

    qDebug() << "Загружено подключений:" << m_connections.size();
    emit connectionsChanged();
}

bool ConnectionManager::saveConnections()
{
    ensureConfigDirExists();

    QJsonArray array;
    for (const Connection &conn : m_connections) {
        array.append(conn.toJson());
    }

    QJsonDocument doc(array);
    QString filePath = getConnectionsFilePath();
    QSaveFile file(filePath);

    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Не удалось сохранить файл подключений:" << filePath;
        return false;
    }

    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        qWarning() << "Не удалось полностью записать файл подключений:" << filePath;
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!file.commit()) {
        qWarning() << "Не удалось атомарно сохранить файл подключений:" << filePath;
        return false;
    }

    qDebug() << "Сохранено подключений:" << m_connections.size();
    return true;
}

void ConnectionManager::addConnection(const Connection &connection)
{
    m_connections.append(connection);
    saveConnections();
    emit connectionsChanged();
}

void ConnectionManager::removeConnection(const QString &id)
{
    for (int i = 0; i < m_connections.size(); ++i) {
        if (m_connections[i].getId() == id) {
            m_connections.removeAt(i);
            saveConnections();
            emit connectionsChanged();
            return;
        }
    }
}

void ConnectionManager::updateConnection(const Connection &connection)
{
    for (int i = 0; i < m_connections.size(); ++i) {
        if (m_connections[i].getId() == connection.getId()) {
            m_connections[i] = connection;
            saveConnections();
            emit connectionsChanged();
            return;
        }
    }
}

Connection ConnectionManager::getConnection(const QString &id) const
{
    for (const Connection &conn : m_connections) {
        if (conn.getId() == id) {
            return conn;
        }
    }
    return Connection();
}

bool ConnectionManager::hasPlaintextPasswords() const
{
    for (const Connection &connection : m_connections) {
        if (!connection.getPassword().isEmpty()) {
            return true;
        }
    }
    return false;
}

bool ConnectionManager::clearPlaintextPasswords()
{
    const QList<Connection> previous = m_connections;
    for (Connection &connection : m_connections) {
        connection.setPassword(QString());
    }
    if (!saveConnections()) {
        m_connections = previous;
        return false;
    }
    return true;
}

QList<Connection> ConnectionManager::getConnections() const
{
    return m_connections;
}
