#include "sftpmanager.h"
#include "legacyssh.h"
#include "sshpassproxy.h"
#include <QDebug>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDate>
#include <QElapsedTimer>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QStandardPaths>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/time.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

namespace {
bool connectWithTimeout(int socketFd, const sockaddr *address, socklen_t addressLength)
{
    const int originalFlags = fcntl(socketFd, F_GETFL, 0);
    if (originalFlags < 0 || fcntl(socketFd, F_SETFL, originalFlags | O_NONBLOCK) < 0) {
        return false;
    }

    int connectResult = ::connect(socketFd, address, addressLength);
    if (connectResult < 0 && errno == EINPROGRESS) {
        pollfd descriptor{};
        descriptor.fd = socketFd;
        descriptor.events = POLLOUT;
        connectResult = poll(&descriptor, 1, 10000);
        if (connectResult > 0) {
            int socketError = 0;
            socklen_t errorLength = sizeof(socketError);
            if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &socketError, &errorLength) < 0 ||
                socketError != 0) {
                connectResult = -1;
            } else {
                connectResult = 0;
            }
        } else {
            connectResult = -1;
            errno = ETIMEDOUT;
        }
    }

    const int savedError = errno;
    const int restoreResult = fcntl(socketFd, F_SETFL, originalFlags);
    errno = savedError;
    return connectResult == 0 && restoreResult == 0;
}
}

SftpManager::SftpManager(QObject *parent)
    : QObject(parent)
    , m_session(nullptr)
    , m_sftp(nullptr)
    , m_socket(-1)
    , m_isConnected(false)
{
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
                                const QString &password, const QString &privateKeyPath,
                                bool allowLegacyHostKeys)
{
    if (m_isConnected) {
        disconnect();
    }

    m_lastError.clear();

    if (allowLegacyHostKeys) {
        m_legacyMode = true;
        m_host = host;
        m_port = port;
        m_user = user;
        m_password = password;
        m_privateKeyPath = privateKeyPath;
        m_allowLegacyHostKeys = true;

        QByteArray output;
        if (!runLegacySftpBatch({"pwd"}, &output)) {
            m_legacyMode = false;
            m_password.clear();
            m_privateKeyPath.clear();
            return false;
        }

        const QRegularExpression homePattern(
            "Remote working directory:\\s*(.+)",
            QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = homePattern.match(QString::fromUtf8(output));
        if (match.hasMatch()) {
            m_homeDirectory = match.captured(1).trimmed();
        }
        if (m_homeDirectory.isEmpty()) {
            setError("SSH подключился, но SFTP-сервер не сообщил домашний каталог");
            m_legacyMode = false;
            m_password.clear();
            m_privateKeyPath.clear();
            return false;
        }

        m_isConnected = true;
        emit connected();
        return true;
    }

    m_legacyMode = false;
    m_password.clear();
    m_privateKeyPath.clear();
    struct addrinfo hints;
    struct addrinfo *result = nullptr;
    
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    std::string hostUtf8 = host.toStdString();
    std::string portStr = std::to_string(port);
    
    int res = getaddrinfo(hostUtf8.c_str(), portStr.c_str(), &hints, &result);
    if (res != 0) {
        setError(QString("Не удалось найти сервер «%1»: %2")
                     .arg(host, QString::fromLocal8Bit(gai_strerror(res))));
        return false;
    }

    m_socket = -1;
    int lastConnectError = 0;
    struct addrinfo *rp;
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        m_socket = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (m_socket == -1) {
            continue;
        }

        if (connectWithTimeout(m_socket, rp->ai_addr, rp->ai_addrlen)) {
            break;
        }

        lastConnectError = errno;
        close(m_socket);
        m_socket = -1;
    }

    freeaddrinfo(result);

    if (m_socket == -1) {
        setError(QString("Не удалось подключиться к серверу %1:%2: %3")
                     .arg(host)
                     .arg(port)
                     .arg(lastConnectError == 0
                              ? QString("превышено время ожидания")
                              : QString::fromLocal8Bit(std::strerror(lastConnectError))));
        return false;
    }

    timeval socketTimeout{};
    socketTimeout.tv_sec = 30;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &socketTimeout, sizeof(socketTimeout));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, &socketTimeout, sizeof(socketTimeout));

    m_session = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
    if (!m_session) {
        setError("Не удалось инициализировать сессию libssh2");
        close(m_socket);
        m_socket = -1;
        return false;
    }

    libssh2_session_set_blocking(m_session, 1);

    if (allowLegacyHostKeys &&
        (libssh2_session_method_pref(m_session, LIBSSH2_METHOD_HOSTKEY, "ssh-rsa") != 0 ||
         libssh2_session_method_pref(m_session, LIBSSH2_METHOD_KEX,
                                    "diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,"
                                    "diffie-hellman-group-exchange-sha1") != 0 ||
         libssh2_session_method_pref(m_session, LIBSSH2_METHOD_CRYPT_CS,
                                    "aes128-cbc,aes256-cbc,3des-cbc,aes128-ctr,aes256-ctr") != 0 ||
         libssh2_session_method_pref(m_session, LIBSSH2_METHOD_CRYPT_SC,
                                    "aes128-cbc,aes256-cbc,3des-cbc,aes128-ctr,aes256-ctr") != 0 ||
         libssh2_session_method_pref(m_session, LIBSSH2_METHOD_MAC_CS,
                                    "hmac-sha1,hmac-sha2-256") != 0 ||
         libssh2_session_method_pref(m_session, LIBSSH2_METHOD_MAC_SC,
                                    "hmac-sha1,hmac-sha2-256") != 0)) {
        setError("Установленная libssh2 не поддерживает один из алгоритмов старого SSH-сервера");
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    if (libssh2_session_handshake(m_session, m_socket)) {
        char *errorMessage = nullptr;
        int errorLength = 0;
        const int errorCode = libssh2_session_last_error(m_session, &errorMessage, &errorLength, 0);
        const QString details = errorMessage && errorLength > 0
                                    ? QString::fromUtf8(errorMessage, errorLength).trimmed()
                                    : QString("код libssh2 %1").arg(errorCode);
        setError(QString("Ошибка согласования SSH-алгоритмов: %1").arg(details));
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

    int authResult = -1;
    if (!privateKeyPath.isEmpty() && !QFile::exists(privateKeyPath)) {
        setError(QString("Файл приватного ключа не найден: %1").arg(privateKeyPath));
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }
    if (!privateKeyPath.isEmpty()) {
        authResult = libssh2_userauth_publickey_fromfile_ex(
            m_session,
            user.toUtf8().constData(),
            user.toUtf8().length(),
            nullptr,
            privateKeyPath.toUtf8().constData(),
            password.toUtf8().constData()
        );
    } else {
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
        setError(QString("Сервер отклонил аутентификацию для пользователя «%1». "
                         "Проверьте логин, пароль или приватный ключ.")
                     .arg(user));
        libssh2_session_disconnect_ex(m_session, SSH_DISCONNECT_BY_APPLICATION, "Auth failed", "");
        libssh2_session_free(m_session);
        m_session = nullptr;
        close(m_socket);
        m_socket = -1;
        return false;
    }

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

    if (m_legacyMode) {
        m_isConnected = false;
        m_legacyMode = false;
        m_password.clear();
        m_privateKeyPath.clear();
        emit disconnected();
        return;
    }

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
    if (m_legacyMode && m_isConnected) {
        return m_homeDirectory;
    }
    if (!m_isConnected || !m_session) {
        setError("SFTP не подключен");
        return QString();
    }

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(m_session);
    if (!channel) {
        setError("Не удалось открыть SSH канал");
        return QString();
    }

    if (libssh2_channel_exec(channel, "echo $HOME") != 0) {
        setError("Не удалось выполнить команду");
        libssh2_channel_free(channel);
        return QString();
    }

    char buffer[1024];
    QString homePath;
    int bytesRead;
    
    while ((bytesRead = libssh2_channel_read(channel, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';
        homePath += QString::fromUtf8(buffer);
    }

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);

    homePath = homePath.trimmed();
    
    qDebug() << "SftpManager: Home directory:" << homePath;
    return homePath;
}

QString SftpManager::executeCommand(const QString &command)
{
    if (m_legacyMode && m_isConnected) {
        return runLegacySshCommand(command);
    }
    if (!m_isConnected || !m_session) {
        setError("SFTP не подключен");
        return QString();
    }

    LIBSSH2_CHANNEL *channel = libssh2_channel_open_session(m_session);
    if (!channel) {
        setError("Не удалось открыть SSH канал");
        return QString();
    }

    if (libssh2_channel_exec(channel, command.toUtf8().constData()) != 0) {
        setError(QString("Не удалось выполнить команду: %1").arg(command));
        libssh2_channel_free(channel);
        return QString();
    }

    char buffer[4096];
    QString output;
    int bytesRead;
    
    while ((bytesRead = libssh2_channel_read(channel, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytesRead] = '\0';
        output += QString::fromUtf8(buffer);
    }

    libssh2_channel_close(channel);
    libssh2_channel_free(channel);

    output = output.trimmed();
    
    qDebug() << "SftpManager: executeCommand('" << command << "') ->" << output;
    return output;
}

QList<SftpFileInfo> SftpManager::listDirectory(const QString &remotePath)
{
    QList<SftpFileInfo> fileList;
    if (m_legacyMode && m_isConnected) {
        QByteArray output;
        if (!runLegacySftpBatch({"ls -l " + quoteSftpArgument(remotePath)}, &output)) {
            return fileList;
        }

        const QRegularExpression linePattern(
            "^([bcdlps-][rwxStTs-]{9}\\+?)\\s+\\d+\\s+\\S+\\s+\\S+\\s+"
            "(\\d+)\\s+([A-Za-z]{3})\\s+(\\d{1,2})\\s+(\\S+)\\s+(.+)$");
        const QStringList lines = QString::fromUtf8(output).split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QRegularExpressionMatch match = linePattern.match(line.trimmed());
            if (!match.hasMatch()) {
                continue;
            }
            SftpFileInfo info;
            info.name = match.captured(6);
            info.path = remotePath.endsWith('/')
                            ? remotePath + info.name
                            : remotePath + "/" + info.name;
            info.size = match.captured(2).toLongLong();
            info.isDir = match.captured(1).startsWith('d');
            const QString datePart = match.captured(3) + " " + match.captured(4) + " " + match.captured(5);
            QDateTime modified = QLocale::c().toDateTime(datePart, "MMM d yyyy");
            if (!modified.isValid()) {
                modified = QLocale::c().toDateTime(
                    datePart, "MMM d hh:mm");
                if (modified.isValid()) {
                    modified.setDate(QDate(QDate::currentDate().year(),
                                           modified.date().month(),
                                           modified.date().day()));
                }
            }
            info.modifiedTime = modified;
            fileList.append(info);
        }
        if (fileList.isEmpty() && !output.trimmed().isEmpty()) {
            setError("Не удалось разобрать ответ SFTP при чтении каталога");
            return {};
        }
        emit directoryListed(remotePath, fileList);
        return fileList;
    }
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
    if (m_legacyMode && m_isConnected) {
        const bool localFileExisted = QFileInfo::exists(localPath);
        const QFileInfo remoteInfo(remotePath);
        QString remoteDirectory = remoteInfo.path();
        if (remoteDirectory.isEmpty()) {
            remoteDirectory = ".";
        }
        qint64 remoteSize = -1;
        const QList<SftpFileInfo> remoteFiles = listDirectory(remoteDirectory);
        for (const SftpFileInfo &file : remoteFiles) {
            if (file.name == remoteInfo.fileName()) {
                remoteSize = file.size;
                break;
            }
        }
        if (!runLegacySftpBatch({"get " + quoteSftpArgument(remotePath) + " " +
                                 quoteSftpArgument(localPath)},
                                nullptr, remoteInfo.fileName(), localPath, remoteSize)) {
            if (!localFileExisted) {
                QFile::remove(localPath);
            }
            emit operationFinished(QFileInfo(remotePath).fileName(), false);
            return false;
        }
        const qint64 bytes = QFileInfo(localPath).size();
        emit downloadProgress(remoteInfo.fileName(), bytes, remoteSize >= 0 ? remoteSize : bytes);
        emit operationFinished(QFileInfo(remotePath).fileName(), true);
        return true;
    }
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
        emit operationFinished(remotePath, false);
        return false;
    }

    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    qint64 fileSize = 0;
    if (libssh2_sftp_fstat_ex(sftpHandle, &attrs, 0) == 0 &&
        (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
        fileSize = static_cast<qint64>(attrs.filesize);
    }

    QFile localFile(localPath);
    if (!localFile.open(QIODevice::WriteOnly)) {
        setError(QString("Не удалось открыть локальный файл для записи: %1").arg(localPath));
        libssh2_sftp_close(sftpHandle);
        emit operationFinished(remotePath, false);
        return false;
    }

    char buffer[32768];
    int bytesRead;
    bool success = true;
    qint64 totalRead = 0;

    QFileInfo fileInfo(remotePath);
    QString fileName = fileInfo.fileName();

    emit downloadProgress(fileName, 0, fileSize);
    while ((bytesRead = libssh2_sftp_read(sftpHandle, buffer, sizeof(buffer))) > 0) {
        if (localFile.write(buffer, bytesRead) != bytesRead) {
            setError("Ошибка записи в локальный файл");
            success = false;
            break;
        }
        
        totalRead += bytesRead;
        emit downloadProgress(fileName, totalRead, fileSize);
    }

    if (bytesRead < 0 && success) {
        setError(QString("Ошибка чтения удаленного файла %1 (код %2)")
                     .arg(remotePath)
                     .arg(libssh2_session_last_errno(m_session)));
        success = false;
    }
    if (success && fileSize > 0 && totalRead != fileSize) {
        setError(QString("Скачан неполный файл %1: %2 из %3 байт")
                     .arg(remotePath).arg(totalRead).arg(fileSize));
        success = false;
    }

    localFile.close();
    libssh2_sftp_close(sftpHandle);

    if (success) {
        qDebug() << "SftpManager: Файл успешно скачан:" << remotePath;
        emit operationFinished(fileName, true);
    } else {
        QFile::remove(localPath);
        emit operationFinished(fileName, false);
    }
    return success;
}

bool SftpManager::uploadFile(const QString &localPath, const QString &remotePath)
{
    if (m_legacyMode && m_isConnected) {
        if (!runLegacySftpBatch({"put " + quoteSftpArgument(localPath) + " " +
                                 quoteSftpArgument(remotePath)})) {
            emit operationFinished(QFileInfo(localPath).fileName(), false);
            return false;
        }
        const qint64 bytes = QFileInfo(localPath).size();
        emit uploadProgress(QFileInfo(localPath).fileName(), bytes, bytes);
        emit operationFinished(QFileInfo(localPath).fileName(), true);
        return true;
    }
    if (!m_isConnected || !m_sftp) {
        setError("SFTP не подключен");
        return false;
    }

    QFile localFile(localPath);
    if (!localFile.open(QIODevice::ReadOnly)) {
        setError(QString("Не удалось открыть локальный файл для чтения: %1").arg(localPath));
        emit operationFinished(localPath, false);
        return false;
    }

    qint64 fileSize = localFile.size();

    LIBSSH2_SFTP_HANDLE *sftpHandle = libssh2_sftp_open(
        m_sftp, remotePath.toUtf8().constData(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH
    );
    if (!sftpHandle) {
        setError(QString("Не удалось создать удаленный файл: %1").arg(remotePath));
        localFile.close();
        emit operationFinished(localPath, false);
        return false;
    }

    char buffer[32768];
    qint64 bytesRead;
    bool success = true;
    qint64 totalWritten = 0;

    QFileInfo fileInfo(localPath);
    QString fileName = fileInfo.fileName();

    while ((bytesRead = localFile.read(buffer, sizeof(buffer))) > 0) {
        qint64 remaining = bytesRead;
        char *writePtr = buffer;

        while (remaining > 0) {
            int rc = libssh2_sftp_write(sftpHandle, writePtr, remaining);
            if (rc < 0) {
                setError(QString("Ошибка записи в удаленный файл (код %1)").arg(rc));
                success = false;
                break;
            }
            totalWritten += rc;
            remaining -= rc;
            writePtr += rc;
        }

        if (!success) break;

        emit uploadProgress(fileName, totalWritten, fileSize);
    }

    localFile.close();
    libssh2_sftp_close(sftpHandle);

    if (success) {
        qDebug() << "SftpManager: Файл успешно загружен:" << remotePath;
        emit operationFinished(fileName, true);
    } else {
        emit operationFinished(fileName, false);
    }
    return success;
}

bool SftpManager::deleteFile(const QString &remotePath)
{
    if (m_legacyMode && m_isConnected) {
        return runLegacySftpBatch({"rm " + quoteSftpArgument(remotePath)});
    }
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
    if (m_legacyMode && m_isConnected) {
        return runLegacySftpBatch({"mkdir " + quoteSftpArgument(remotePath)});
    }
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

QString SftpManager::quoteSftpArgument(const QString &argument)
{
    QString escaped = argument;
    escaped.replace('\\', "\\\\");
    escaped.replace('"', "\\\"");
    escaped.replace('\n', "\\n");
    escaped.replace('\r', "\\r");
    return "\"" + escaped + "\"";
}

bool SftpManager::runLegacySftpBatch(const QStringList &commands, QByteArray *standardOutput,
                                     const QString &progressFile,
                                     const QString &localProgressPath,
                                     qint64 progressTotal)
{
    m_lastError.clear();
    QString opensslError;
    const QString opensslConfig = legacySshOpenSslConfigPath(&opensslError);
    if (!opensslError.isEmpty()) {
        setError(opensslError);
        return false;
    }

    const QString sftpPath = QStandardPaths::findExecutable("sftp");
    if (sftpPath.isEmpty()) {
        setError("Не найден системный клиент OpenSSH sftp");
        return false;
    }

    QStringList arguments;
    arguments << "-P" << QString::number(m_port);
    for (const QString &option : legacySshOptions()) {
        arguments << option;
    }
    arguments << "-o" << "StrictHostKeyChecking=accept-new";
    arguments << "-o" << "NumberOfPasswordPrompts=1";
    arguments << "-o" << "BatchMode=no";
    if (!m_privateKeyPath.isEmpty()) {
        arguments << "-i" << m_privateKeyPath;
    }
    const bool usePasswordAuthentication = !m_password.isEmpty() && m_privateKeyPath.isEmpty();
    if (usePasswordAuthentication) {
        arguments << "-o" << "PreferredAuthentications=password,keyboard-interactive";
        arguments << "-o" << "PubkeyAuthentication=no";
    }
    arguments << (m_user + "@" + m_host);

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove("SSHPASS");
    if (!opensslConfig.isEmpty()) {
        environment.insert("OPENSSL_CONF", opensslConfig);
    }
    QString executable = sftpPath;
    QLocalServer passwordServer;
    QString passwordSocketName;
    if (!m_password.isEmpty() && m_privateKeyPath.isEmpty()) {
        const QString sshpassPath = QStandardPaths::findExecutable("sshpass");
        if (sshpassPath.isEmpty()) {
            setError("Для legacy-подключения по паролю нужен установленный sshpass");
            return false;
        }
        passwordSocketName = createSshpassProxySocketName();
        passwordServer.setSocketOptions(QLocalServer::UserAccessOption);
        if (!passwordServer.listen(passwordSocketName)) {
            setError("Не удалось создать защищённый канал для передачи пароля SFTP");
            return false;
        }
        QStringList sshpassArguments{sftpPath};
        sshpassArguments.append(arguments);
        arguments = makeSshpassProxyArguments(passwordSocketName, sshpassPath, sshpassArguments);
        executable = QCoreApplication::applicationFilePath();
    }
    process.setProcessEnvironment(environment);
    process.setProgram(executable);
    process.setArguments(arguments);
    process.start();
    if (!process.waitForStarted(5000)) {
        setError(QString("Не удалось запустить OpenSSH sftp: %1").arg(process.errorString()));
        if (!passwordSocketName.isEmpty()) {
            QLocalServer::removeServer(passwordSocketName);
        }
        return false;
    }
    if (!passwordSocketName.isEmpty()) {
        QLocalSocket *client = waitForSshpassProxy(&passwordServer, process.processId(), 5000);
        if (!client) {
            process.kill();
            process.waitForFinished();
            QLocalServer::removeServer(passwordSocketName);
            setError("Не удалось передать пароль вспомогательному процессу SFTP");
            return false;
        }
        if (!sendSshpassProxyPassword(client, m_password)) {
            process.kill();
            process.waitForFinished();
            QLocalServer::removeServer(passwordSocketName);
            setError("Не удалось передать пароль вспомогательному процессу SFTP");
            return false;
        }
        if (client) {
            client->disconnectFromServer();
        }
        passwordServer.close();
        QLocalServer::removeServer(passwordSocketName);
    }
    QStringList inputCommands = commands;
    inputCommands << "quit";
    process.write((inputCommands.join('\n') + '\n').toUtf8());
    process.closeWriteChannel();

    QElapsedTimer transferTimer;
    transferTimer.start();
    qint64 lastReportedSize = -1;
    QByteArray output;
    QByteArray errorOutput;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(250);
        output += process.readAllStandardOutput();
        errorOutput += process.readAllStandardError();
        if (!progressFile.isEmpty()) {
            const qint64 currentSize = QFileInfo(localProgressPath).size();
            if (currentSize != lastReportedSize) {
                emit downloadProgress(progressFile, currentSize, progressTotal);
                lastReportedSize = currentSize;
            }
        }
        if (transferTimer.elapsed() >= 300000) {
            process.kill();
            process.waitForFinished();
            setError("Операция SFTP прервана: превышено время ожидания (5 минут)");
            return false;
        }
    }
    output += process.readAllStandardOutput();
    errorOutput += process.readAllStandardError();
    if (standardOutput) {
        *standardOutput = output;
    }
    QString details = QString::fromLocal8Bit(errorOutput).trimmed();
    const QString stdoutText = QString::fromUtf8(output).trimmed();
    const QRegularExpression operationError(
        "(Couldn't|Failure|Permission denied|No such file|not found|Connection closed)",
        QRegularExpression::CaseInsensitiveOption);
    if (operationError.match(stdoutText).hasMatch()) {
        details += (details.isEmpty() ? QString() : "\n") + stdoutText;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        operationError.match(details).hasMatch()) {
        if (details.isEmpty()) {
            details = QString("OpenSSH sftp завершился с кодом %1").arg(process.exitCode());
        }
        setError(details);
        return false;
    }
    return true;
}

QString SftpManager::runLegacySshCommand(const QString &command)
{
    m_lastError.clear();
    QString opensslError;
    const QString opensslConfig = legacySshOpenSslConfigPath(&opensslError);
    if (!opensslError.isEmpty()) {
        setError(opensslError);
        return {};
    }

    const QString sshPath = QStandardPaths::findExecutable("ssh");
    if (sshPath.isEmpty()) {
        setError("Не найден системный клиент OpenSSH ssh");
        return {};
    }

    QStringList sshArguments;
    sshArguments << "-p" << QString::number(m_port);
    for (const QString &option : legacySshOptions()) {
        sshArguments << option;
    }
    sshArguments << "-o" << "StrictHostKeyChecking=accept-new";
    sshArguments << "-o" << "NumberOfPasswordPrompts=1";
    sshArguments << "-o" << "BatchMode=no";
    if (!m_privateKeyPath.isEmpty()) {
        sshArguments << "-i" << m_privateKeyPath;
    }
    const bool usePasswordAuthentication = !m_password.isEmpty() && m_privateKeyPath.isEmpty();
    if (usePasswordAuthentication) {
        sshArguments << "-o" << "PreferredAuthentications=password,keyboard-interactive";
        sshArguments << "-o" << "PubkeyAuthentication=no";
    }
    sshArguments << (m_user + "@" + m_host) << command;

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove("SSHPASS");
    if (!opensslConfig.isEmpty()) {
        environment.insert("OPENSSL_CONF", opensslConfig);
    }
    QString executable = sshPath;
    QLocalServer passwordServer;
    QString passwordSocketName;
    if (!m_password.isEmpty() && m_privateKeyPath.isEmpty()) {
        const QString sshpassPath = QStandardPaths::findExecutable("sshpass");
        if (sshpassPath.isEmpty()) {
            setError("Для legacy-подключения по паролю нужен установленный sshpass");
            return {};
        }
        passwordSocketName = createSshpassProxySocketName();
        passwordServer.setSocketOptions(QLocalServer::UserAccessOption);
        if (!passwordServer.listen(passwordSocketName)) {
            setError("Не удалось создать защищённый канал для передачи пароля SSH");
            return {};
        }
        QStringList sshpassArguments{sshPath};
        sshpassArguments.append(sshArguments);
        sshArguments = makeSshpassProxyArguments(passwordSocketName, sshpassPath, sshpassArguments);
        executable = QCoreApplication::applicationFilePath();
    }
    process.setProcessEnvironment(environment);
    process.setProgram(executable);
    process.setArguments(sshArguments);
    process.start();
    if (!process.waitForStarted(5000)) {
        setError(QString("Не удалось запустить OpenSSH ssh: %1").arg(process.errorString()));
        if (!passwordSocketName.isEmpty()) {
            QLocalServer::removeServer(passwordSocketName);
        }
        return {};
    }
    if (!passwordSocketName.isEmpty()) {
        QLocalSocket *client = waitForSshpassProxy(&passwordServer, process.processId(), 5000);
        if (!client) {
            process.kill();
            process.waitForFinished();
            QLocalServer::removeServer(passwordSocketName);
            setError("Не удалось передать пароль вспомогательному процессу SSH");
            return {};
        }
        if (!sendSshpassProxyPassword(client, m_password)) {
            process.kill();
            process.waitForFinished();
            QLocalServer::removeServer(passwordSocketName);
            setError("Не удалось передать пароль вспомогательному процессу SSH");
            return {};
        }
        if (client) {
            client->disconnectFromServer();
        }
        passwordServer.close();
        QLocalServer::removeServer(passwordSocketName);
    }
    QElapsedTimer commandTimer;
    commandTimer.start();
    QByteArray output;
    QByteArray errorOutput;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(250);
        output += process.readAllStandardOutput();
        errorOutput += process.readAllStandardError();
        if (commandTimer.elapsed() >= 300000) {
            process.kill();
            process.waitForFinished();
            setError("SSH-команда прервана: превышено время ожидания (5 минут)");
            return {};
        }
    }

    output += process.readAllStandardOutput();
    errorOutput += process.readAllStandardError();
    const QString errorText = QString::fromLocal8Bit(errorOutput).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        setError(errorText.isEmpty()
                     ? QString("OpenSSH ssh завершился с кодом %1").arg(process.exitCode())
                     : errorText);
        return {};
    }
    return QString::fromUtf8(output).trimmed();
}