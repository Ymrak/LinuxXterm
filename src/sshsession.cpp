#include "sshsession.h"
#include <QDebug>
#include <QFile>

SshSession::SshSession(QObject *parent)
    : QObject(parent)
    , m_socket(-1)
    , m_session(nullptr)
    , m_sftpSession(nullptr)
    , m_connected(false)
{
    // Инициализация libssh2
    libssh2_init(0);
}

SshSession::~SshSession()
{
    disconnect();
    libssh2_exit();
}

bool SshSession::connect(const Connection &connection)
{
    if (m_connected) {
        disconnect();
    }

    if (connection.getUseKey()) {
        return connectByKey(connection.getHost(), connection.getPort(), 
                           connection.getLogin(), connection.getPrivateKeyPath());
    } else {
        return connectByPassword(connection.getHost(), connection.getPort(), 
                                connection.getLogin(), connection.getPassword());
    }
}

bool SshSession::connectByPassword(const QString &host, int port, 
                                   const QString &login, const QString &password)
{
    // Создание сокета
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        emit error("Не удалось создать сокет");
        return false;
    }

    // Настройка адреса
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.toStdString().c_str(), &addr.sin_addr);

    // Подключение
    if (::connect(m_socket, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        emit error("Не удалось подключиться к " + host + ":" + QString::number(port));
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Создание сессии libssh2
    m_session = libssh2_session_init();
    if (!m_session) {
        emit error("Не удалось инициализировать SSH сессию");
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Установка неблокирующего режима (опционально)
    libssh2_session_set_blocking(m_session, 1);

    // handshake
    if (libssh2_session_handshake(m_session, m_socket) != 0) {
        emit error("Ошибка handshake с сервером");
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Аутентификация по паролю
    if (libssh2_userauth_password(m_session, login.toStdString().c_str(), 
                                  password.toStdString().c_str()) != 0) {
        emit error("Ошибка аутентификации: неверный логин или пароль");
        libssh2_session_disconnect(m_session, "Authentication failed");
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    m_connected = true;
    emit connected();
    qDebug() << "Подключено к" << host << "по паролю";
    return true;
}

bool SshSession::connectByKey(const QString &host, int port, 
                              const QString &login, const QString &privateKeyPath)
{
    // Проверка существования файла ключа
    QFile keyFile(privateKeyPath);
    if (!keyFile.exists()) {
        emit error("Файл ключа не найден: " + privateKeyPath);
        return false;
    }

    // Создание сокета
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0) {
        emit error("Не удалось создать сокет");
        return false;
    }

    // Настройка адреса
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.toStdString().c_str(), &addr.sin_addr);

    // Подключение
    if (::connect(m_socket, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        emit error("Не удалось подключиться к " + host + ":" + QString::number(port));
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Создание сессии libssh2
    m_session = libssh2_session_init();
    if (!m_session) {
        emit error("Не удалось инициализировать SSH сессию");
        close(m_socket);
        m_socket = -1;
        return false;
    }

    libssh2_session_set_blocking(m_session, 1);

    // handshake
    if (libssh2_session_handshake(m_session, m_socket) != 0) {
        emit error("Ошибка handshake с сервером");
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // Аутентификация по ключу
    if (libssh2_userauth_publickey_fromfile(m_session, login.toStdString().c_str(),
                                            nullptr,
                                            privateKeyPath.toStdString().c_str(),
                                            nullptr) != 0) {
        emit error("Ошибка аутентификации по ключу");
        libssh2_session_disconnect(m_session, "Authentication failed");
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    m_connected = true;
    emit connected();
    qDebug() << "Подключено к" << host << "по ключу";
    return true;
}

void SshSession::disconnect()
{
    if (m_sftpSession) {
        libssh2_sftp_shutdown(m_sftpSession);
        m_sftpSession = nullptr;
    }

    if (m_session) {
        libssh2_session_disconnect(m_session, "Client disconnecting");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }

    if (m_socket >= 0) {
        close(m_socket);
        m_socket = -1;
    }

    if (m_connected) {
        m_connected = false;
        emit disconnected();
    }
}

bool SshSession::isConnected() const
{
    return m_connected;
}

LIBSSH2_SESSION* SshSession::getSession() const
{
    return m_session;
}

LIBSSH2_SFTP* SshSession::getSftpSession() const
{
    return m_sftpSession;
}
