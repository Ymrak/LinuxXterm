#include "connectionmanager.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QStandardPaths>
#include <QDebug>

ConnectionManager::ConnectionManager(QObject *parent)
    : QObject(parent)
{
    loadConnections();
}

ConnectionManager::~ConnectionManager()
{
    saveConnections();
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

void ConnectionManager::saveConnections()
{
    ensureConfigDirExists();

    QJsonArray array;
    for (const Connection &conn : m_connections) {
        array.append(conn.toJson());
    }

    QJsonDocument doc(array);
    QString filePath = getConnectionsFilePath();
    QFile file(filePath);

    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Не удалось сохранить файл подключений:" << filePath;
        return;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    qDebug() << "Сохранено подключений:" << m_connections.size();
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

QList<Connection> ConnectionManager::getConnections() const
{
    return m_connections;
}
