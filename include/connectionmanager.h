#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

#include "connection.h"
#include <QObject>
#include <QList>
#include <QJsonArray>

class ConnectionManager : public QObject
{
    Q_OBJECT

public:
    explicit ConnectionManager(QObject *parent = nullptr);
    ~ConnectionManager();

    // Загрузка подключений из файла
    void loadConnections();

    // Сохранение подключений в файл
    void saveConnections();

    // Операции со списком
    void addConnection(const Connection &connection);
    void removeConnection(const QString &id);
    void updateConnection(const Connection &connection);
    Connection getConnection(const QString &id) const;

    // Получение всего списка
    QList<Connection> getConnections() const;

signals:
    // Сигнал об изменении списка (для обновления UI)
    void connectionsChanged();

private:
    QString getConfigPath() const;
    QString getConnectionsFilePath() const;
    void ensureConfigDirExists();

    QList<Connection> m_connections;
};

#endif // CONNECTIONMANAGER_H
