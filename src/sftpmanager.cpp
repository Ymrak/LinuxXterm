#include "sftpmanager.h"
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>

SftpManager::SftpManager(QObject *parent)
    : QObject(parent)
    , m_session(nullptr)
    , m_sftp(nullptr)
    , m_socket(-1)
    , m_isConnected(false)
{
    // Инициализация libssh2 при первом использовании (потокобезопасная)
    static bool libssh2Initialized = false;
    if (!libssh2Initialized) {
        libssh2_init(0);
        libssh2Initialized = true;
        qDebug() << "SftpManager: libssh2 initialized";
    }
}

SftpManager::~SftpManager()
{
    disconnect();
}

bool SftpManager::connectToHost(const QString &host, int port, const QString &user, 
                                const QString &password, const QString &privateKeyPath)
{
    if (m_isConnected) {
        disconnect();
    }

    m_lastError.clear();

    // 1. Резолвинг адреса через getaddrinfo (поддержка доменов и IPv4/IPv6)
    struct addrinfo hints;
    struct addrinfo *result = nullptr;
    
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;     // IPv4 или IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP
    
    std::string hostUtf8 = host.toStdString();
    std::string portStr = std::to_string(port);
    
    int res = getaddrinfo(hostUtf8.c_str(), portStr.c_str(), &hints, &result);
    if (res != 0) {
        setError(QString("Не удалось резолвить адрес %1: %2").arg(host).arg(gai_strerror(res)));
        return false;
    }

    // 2. Создание сокета и подключение (пытаемся по всем полученным адресам)
    m_socket = -1;
    struct addrinfo *rp;
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        m_socket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (m_socket == -1) {
            continue; // Пробуем следующий адрес
        }

        if (::connect(m_socket, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; // Успешно подключились
        }

        close(m_socket);
        m_socket = -1;
    }

    freeaddrinfo(result); // Освобождаем ресурсы getaddrinfo

    if (m_socket == -1) {
        setError(QString("Не удалось подключиться к %1:%2").arg(host).arg(port));
        return false;
    }

    // 3. Инициализация сессии libssh2
    m_session = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
    if (!m_session) {
        setError("Не удалось инициализировать сессию libssh2");
        close(m_socket);
        m_socket = -1;
        return false;
    }

    libssh2_session_set_blocking(m_session, 1);

    // 4. Рукопожатие (handshake)
    if (libssh2_session_handshake(m_session, m_socket)) {
        setError("Ошибка рукопожатия SSH");
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // 5. Аутентификация
    int authResult = -1;
    if (!privateKeyPath.isEmpty() && QFile::exists(privateKeyPath)) {
        // Аутентификация по ключу
        authResult = libssh2_userauth_publickey_fromfile_ex(
            m_session,
            user.toUtf8().constData(),
            user.toUtf8().length(),
            nullptr, // публичный ключ (можно оставить nullptr, libssh2 найдёт его сам)
            privateKeyPath.toUtf8().constData(),
            password.toUtf8().constData() // пароль для расшифровки приватного ключа, если есть
        );
    } else {
        // Аутентификация по паролю
        authResult = libssh2_userauth_password_ex(
            m_session,
            user.toUtf8().constData(),
            user.toUtf8().length(),
            password.toUtf8().constData(),
            password.toUtf8().length(),
            nullptr
        );
    }

    if (authResult != 0) {
        setError("Ошибка аутентификации. Проверьте логин, пароль или ключ.");
        libssh2_session_disconnect_ex(m_session, SSH_DISCONNECT_BY_APPLICATION, "Auth failed", "");
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    // 6. Инициализация SFTP подсистемы
    m_sftp = libssh2_sftp_init(m_session);
    if (!m_sftp) {
        setError("Не удалось инициализировать SFTP подсистему");
        libssh2_session_disconnect_ex(m_session, SSH_DISCONNECT_BY_APPLICATION, "SFTP init failed", "");
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    m_isConnected = true;
    qDebug() << "SftpManager: Успешно подключено к" << host;
    emit connected();
    return true;
}

void SftpManager::disconnect()
{
    if (!m_isConnected) return;

    if (m_sftp) {
        libssh2_sftp_shutdown(m_sftp);
        m_sftp = nullptr;
    }
    if (m_session) {
        libssh2_session_disconnect_ex(m_session, SSH_DISCONNECT_BY_APPLICATION, "Normal shutdown", "");
        libssh2_session_free(m_session);
        m_session = nullptr;
    }
    if (m_socket != -1) {
        close(m_socket);
        m_socket = -1;
    }

    m_isConnected = false;
    qDebug() << "SftpManager: Отключено";
    emit disconnected();
}

QString SftpManager::getHomeDirectory()
{
    if (!m_isConnected || !m_session) {
        setError("SFTP не подключен");
        return QString();
    }

    // Открываем SSH канал для выполнения команды
    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(m_session);
    if (!channel) {
        setError("Не удалось открыть SSH канал");
        return QString();
    }

    // Выполняем команду echo $HOME
    if (libssh2_channel_exec(channel, "echo $HOME") != 0) {
        setError("Не удалось выполнить команду");
        libssh2_channel_free(channel);
        return QString();
    }

    // Читаем результат
    char buffer[1024];
    QString homePath;
    int bytesRead;
    
    while ((bytesRead = libssh2_channel_read(channel, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';
        homePath += QString::fromUtf8(buffer);
    }

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);

    // Убираем перенос строки в конце
    homePath = homePath.trimmed();
    
    qDebug() << "SftpManager: Home directory:" << homePath;
    return homePath;
}

QList<SftpFileInfo> SftpManager::listDirectory(const QString &remotePath)
{
    QList<SftpFileInfo> fileList;
    if (!m_isConnected || !m_sftp) {
        setError("SFTP не подключен");
        return fileList;
    }

    LIBSSH2_SFTP_HANDLE *sftpHandle = libssh2_sftp_opendir(m_sftp, remotePath.toUtf8().constData());
    if (!sftpHandle) {
        setError(QString("Не удалось открыть директорию: %1").arg(remotePath));
        return fileList;
    }

    char mem[512];
    char longentry[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (libssh2_sftp_readdir_ex(sftpHandle, mem, sizeof(mem), longentry, sizeof(longentry), &attrs) > 0) {
        QString fileName = QString::fromUtf8(mem);
        
        // Пропускаем текущую и родительскую директории
        if (fileName == "." || fileName == "..") {
            continue;
        }

        SftpFileInfo info;
        info.name = fileName;
        info.path = remotePath.endsWith('/') ? remotePath + fileName : remotePath + "/" + fileName;
        info.size = attrs.filesize;
        info.isDir = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        info.modifiedTime = QDateTime::fromSecsSinceEpoch(attrs.mtime);

        fileList.append(info);
    }

    libssh2_sftp_closedir(sftpHandle);
    
    emit directoryListed(remotePath, fileList);
    return fileList;
}

bool SftpManager::downloadFile(const QString &remotePath, const QString &localPath)
{
    if (!m_isConnected || !m_sftp) {
        setError("SFTP не подключен");
        return false;
    }

    LIBSSH2_SFTP_HANDLE *sftpHandle = libssh2_sftp_open(
        m_sftp, remotePath.toUtf8().constData(), 
        LIBSSH2_FXF_READ, 0
    );
    if (!sftpHandle) {
        setError(QString("Не удалось открыть удаленный файл: %1").arg(remotePath));
        return false;
    }

    QFile localFile(localPath);
    if (!localFile.open(QIODevice::WriteOnly)) {
        setError(QString("Не удалось открыть локальный файл для записи: %1").arg(localPath));
        libssh2_sftp_close(sftpHandle);
        return false;
    }

    char buffer[32768];
    int bytesRead;
    bool success = true;

    while ((bytesRead = libssh2_sftp_read(sftpHandle, buffer, sizeof(buffer))) > 0) {
        if (localFile.write(buffer, bytesRead) != bytesRead) {
            setError("Ошибка записи в локальный файл");
            success = false;
            break;
        }
    }

    localFile.close();
    libssh2_sftp_close(sftpHandle);

    if (success) {
        qDebug() << "SftpManager: Файл успешно скачан:" << remotePath;
    }
    return success;
}

bool SftpManager::uploadFile(const QString &localPath, const QString &remotePath)
{
    if (!m_isConnected || !m_sftp) {
        setError("SFTP не подключен");
        return false;
    }

    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        setError(QString("Не удалось открыть локальный файл для чтения: %1").arg(localPath));
        return false;
    }

    LIBSSH2_SFTP_HANDLE *sftpHandle = libssh2_sftp_open(
        m_sftp, remotePath.toUtf8().constData(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH
    );
    if (!sftpHandle) {
        setError(QString("Не удалось создать удаленный файл: %1").arg(remotePath));
        localFile.close();
        return false;
    }

    char buffer[32768];
    qint64 bytesRead;
    bool success = true;
    qint64 totalWritten = 0;
    qint64 fileSize = localFile.size();

    qDebug() << "SftpManager: Начинаем загрузку файла" << localPath << "->" << remotePath;
    qDebug() << "SftpManager: Размер файла:" << fileSize << "байт";

    while ((bytesRead = localFile.read(buffer, sizeof(buffer))) > 0) {
        qint64 bytesWritten = 0;
        qint64 remaining = bytesRead;
        char *writePtr = buffer;

        // Обрабатываем частичную запись (libssh2_sftp_write может записать меньше байт)
        while (remaining > 0) {
            int rc = libssh2_sftp_write(sftpHandle, writePtr, remaining);
            if (rc < 0) {
                // Реальная ошибка
                setError(QString("Ошибка записи в удаленный файл (код %1)").arg(rc));
                success = false;
                break;
            }
            bytesWritten += rc;
            remaining -= rc;
            writePtr += rc;
            totalWritten += rc;
        }

        if (!success) break;

        // Прогресс загрузки
        int progress = (totalWritten * 100) / fileSize;
        qDebug() << "SftpManager: Прогресс:" << progress << "% (" << totalWritten << "/" << fileSize << ")";
    }

    localFile.close();
    libssh2_sftp_close(sftpHandle);

    if (success) {
        qDebug() << "SftpManager: Файл успешно загружен:" << remotePath << "(" << totalWritten << "байт)";
    }
    return success;
}

bool SftpManager::deleteFile(const QString &remotePath)
{
    if (!m_isConnected || !m_sftp) {
        setError("SFTP не подключен");
        return false;
    }

    QByteArray pathUtf8 = remotePath.toUtf8();
    int rc = libssh2_sftp_unlink_ex(m_sftp, pathUtf8.constData(), pathUtf8.length());
    if (rc != 0) {
        setError(QString("Не удалось удалить файл: %1").arg(remotePath));
        return false;
    }
    
    qDebug() << "SftpManager: Файл удален:" << remotePath;
    return true;
}

bool SftpManager::createDirectory(const QString &remotePath)
{
    if (!m_isConnected || !m_sftp) {
        setError("SFTP не подключен");
        return false;
    }

    QByteArray pathUtf8 = remotePath.toUtf8();
    int rc = libssh2_sftp_mkdir_ex(
        m_sftp, 
        pathUtf8.constData(),
        pathUtf8.length(),
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP | LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH
    );
    if (rc != 0) {
        setError(QString("Не удалось создать директорию: %1").arg(remotePath));
        return false;
    }

    qDebug() << "SftpManager: Директория создана:" << remotePath;
    return true;
}

void SftpManager::setError(const QString &error)
{
    m_lastError = error;
    qWarning() << "SftpManager Error:" << error;
    emit errorOccurred(error);
}