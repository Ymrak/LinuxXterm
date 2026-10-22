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

    void loadConnections();

    bool saveConnections();

    void addConnection(const Connection &connection);
    void removeConnection(const QString &id);
    void updateConnection(const Connection &connection);
    Connection getConnection(const QString &id) const;
    bool hasPlaintextPasswords() const;
    bool clearPlaintextPasswords();

    QList<Connection> getConnections() const;

signals:
    void connectionsChanged();

private:
    QString getConfigPath() const;
    QString getConnectionsFilePath() const;
    void ensureConfigDirExists();

    QList<Connection> m_connections;
};

#endif
