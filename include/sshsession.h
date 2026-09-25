#ifndef SSHSESSION_H
#define SSHSESSION_H

#include <QObject>
#include <QString>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "connection.h"

class SshSession : public QObject
{
    Q_OBJECT

public:
    explicit SshSession(QObject *parent = nullptr);
    ~SshSession();

    // Подключение к серверу
    bool connect(const Connection &connection);
    
    // Отключение
    void disconnect();
    
    // Проверка статуса
    bool isConnected() const;

    // Получение указателей на сессии (для использования в других классах)
    LIBSSH2_SESSION* getSession() const;
    LIBSSH2_SFTP* getSftpSession() const;

signals:
    void connected();
    void disconnected();
    void error(const QString &message);

private:
    bool connectByPassword(const QString &host, int port, const QString &login, const QString &password);
    bool connectByKey(const QString &host, int port, const QString &login, const QString &privateKeyPath);
    
    int m_socket;
    LIBSSH2_SESSION *m_session;
    LIBSSH2_SFTP *m_sftpSession;
    bool m_connected;
};

#endif // SSHSESSION_H
