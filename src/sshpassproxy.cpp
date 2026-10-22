#include "sshpassproxy.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStringList>
#include <QUuid>
#include <QtEndian>
#include <openssl/crypto.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
volatile sig_atomic_t proxyChildPid = -1;

bool readExact(QLocalSocket &socket, char *buffer, qsizetype size)
{
    qsizetype received = 0;
    while (received < size) {
        if (socket.bytesAvailable() == 0 && !socket.waitForReadyRead(5000)) {
            return false;
        }
        const qint64 count = socket.read(buffer + received, size - received);
        if (count <= 0) {
            return false;
        }
        received += count;
    }
    return true;
}

bool writeExact(int descriptor, const QByteArray &data)
{
    qsizetype written = 0;
    while (written < data.size()) {
        const ssize_t count = ::write(descriptor, data.constData() + written,
                                      static_cast<size_t>(data.size() - written));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        written += count;
    }
    return true;
}

int reportError(const char *message)
{
    const size_t length = std::strlen(message);
    ::write(STDERR_FILENO, message, length);
    ::write(STDERR_FILENO, "\n", 1);
    return 1;
}

void forwardSignal(int signalNumber)
{
    const pid_t child = proxyChildPid;
    if (child > 0) {
        ::kill(child, signalNumber);
    }
}
}

QString createSshpassProxySocketName()
{
    return QDir::tempPath() + "/linuxxterm-"
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QStringList makeSshpassProxyArguments(const QString &socketName, const QString &sshpassPath,
                                      const QStringList &sshpassArguments)
{
    QStringList arguments{"--sshpass-proxy", socketName, sshpassPath, "-d", "3"};
    arguments.append(sshpassArguments);
    return arguments;
}

bool isSshpassProxyPeer(QLocalSocket *socket, qint64 expectedProcessId)
{
    if (!socket || expectedProcessId <= 0) {
        return false;
    }

    struct ucred credentials {};
    socklen_t credentialsSize = sizeof(credentials);
    return ::getsockopt(
        static_cast<int>(socket->socketDescriptor()),
        SOL_SOCKET,
        SO_PEERCRED,
        &credentials,
        &credentialsSize
    ) == 0 && credentials.pid == expectedProcessId;
}

QLocalSocket *waitForSshpassProxy(QLocalServer *server, qint64 expectedProcessId,
                                  int timeoutMilliseconds)
{
    if (!server || expectedProcessId <= 0) {
        return nullptr;
    }

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMilliseconds) {
        if (!server->hasPendingConnections()) {
            const int remaining = timeoutMilliseconds - static_cast<int>(timer.elapsed());
            if (!server->waitForNewConnection(remaining)) {
                return nullptr;
            }
        }
        while (server->hasPendingConnections()) {
            QLocalSocket *client = server->nextPendingConnection();
            if (isSshpassProxyPeer(client, expectedProcessId)) {
                return client;
            }
            client->abort();
            client->deleteLater();
        }
    }
    return nullptr;
}

bool sendSshpassProxyPassword(QLocalSocket *socket, const QString &password)
{
    if (!socket || password.isEmpty()) {
        return false;
    }

    QByteArray passwordBytes = password.toUtf8();
    if (passwordBytes.size() > 1024 * 1024) {
        OPENSSL_cleanse(passwordBytes.data(), passwordBytes.size());
        return false;
    }

    QByteArray payload(4, Qt::Uninitialized);
    qToBigEndian(static_cast<quint32>(passwordBytes.size()),
                 reinterpret_cast<uchar *>(payload.data()));
    payload.append(passwordBytes);
    OPENSSL_cleanse(passwordBytes.data(), passwordBytes.size());

    bool sent = socket->write(payload) == payload.size();
    while (sent && socket->bytesToWrite() > 0) {
        sent = socket->waitForBytesWritten(5000);
    }
    OPENSSL_cleanse(payload.data(), payload.size());
    return sent;
}

int runSshpassProxy(int argc, char *argv[])
{
    if (argc < 7 || std::strcmp(argv[1], "--sshpass-proxy") != 0) {
        return reportError("Invalid sshpass proxy arguments");
    }

    QLocalSocket socket;
    socket.connectToServer(QString::fromLocal8Bit(argv[2]));
    if (!socket.waitForConnected(5000)) {
        return reportError("Could not receive the SSH password");
    }

    QByteArray lengthBuffer(4, Qt::Uninitialized);
    if (!readExact(socket, lengthBuffer.data(), lengthBuffer.size())) {
        return reportError("Could not read the SSH password");
    }

    const quint32 passwordSize =
        qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(lengthBuffer.constData()));
    if (passwordSize == 0 || passwordSize > 1024 * 1024) {
        return reportError("Invalid SSH password length");
    }

    QByteArray password(passwordSize, Qt::Uninitialized);
    if (!readExact(socket, password.data(), password.size())) {
        OPENSSL_cleanse(password.data(), password.size());
        return reportError("Could not read the SSH password");
    }
    socket.abort();

    int passwordPipe[2];
    if (::pipe(passwordPipe) != 0) {
        OPENSSL_cleanse(password.data(), password.size());
        return reportError("Could not create a password pipe");
    }

    QByteArray passwordLine = password;
    passwordLine.append('\n');
    OPENSSL_cleanse(password.data(), password.size());
    const int writeFlags = ::fcntl(passwordPipe[1], F_GETFD);
    if (writeFlags == -1
        || ::fcntl(passwordPipe[1], F_SETFD, writeFlags | FD_CLOEXEC) == -1) {
        OPENSSL_cleanse(passwordLine.data(), passwordLine.size());
        ::close(passwordPipe[1]);
        ::close(passwordPipe[0]);
        return reportError("Could not prepare the password pipe");
    }

    QStringList arguments;
    for (int i = 4; i < argc; ++i) {
        arguments.append(QString::fromLocal8Bit(argv[i]));
    }
    bool descriptorUpdated = false;
    for (qsizetype i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments.at(i) == "-d") {
            arguments[i + 1] = QString::number(passwordPipe[0]);
            descriptorUpdated = true;
            break;
        }
    }
    if (!descriptorUpdated) {
        OPENSSL_cleanse(passwordLine.data(), passwordLine.size());
        ::close(passwordPipe[1]);
        ::close(passwordPipe[0]);
        return reportError("sshpass descriptor argument is missing");
    }

    ::unsetenv("SSHPASS");

    const int descriptorFlags = ::fcntl(passwordPipe[0], F_GETFD);
    if (descriptorFlags == -1
        || ::fcntl(passwordPipe[0], F_SETFD, descriptorFlags & ~FD_CLOEXEC) == -1) {
        OPENSSL_cleanse(passwordLine.data(), passwordLine.size());
        ::close(passwordPipe[1]);
        ::close(passwordPipe[0]);
        return reportError("Could not prepare the password pipe");
    }

    std::vector<QByteArray> argumentStorage;
    argumentStorage.reserve(static_cast<size_t>(arguments.size()) + 1);
    argumentStorage.emplace_back(QString::fromLocal8Bit(argv[3]).toLocal8Bit());
    for (const QString &argument : arguments) {
        argumentStorage.emplace_back(argument.toLocal8Bit());
    }

    std::vector<char *> execArguments;
    execArguments.reserve(argumentStorage.size() + 1);
    for (QByteArray &argument : argumentStorage) {
        execArguments.push_back(argument.data());
    }
    execArguments.push_back(nullptr);

    struct sigaction action {};
    action.sa_handler = forwardSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGHUP, &action, nullptr);
    sigaction(SIGQUIT, &action, nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        OPENSSL_cleanse(passwordLine.data(), passwordLine.size());
        ::close(passwordPipe[1]);
        ::close(passwordPipe[0]);
        return reportError("Could not create the sshpass process");
    }
    if (child == 0) {
        ::close(passwordPipe[1]);
        ::execvp(execArguments.front(), execArguments.data());
        reportError("Could not start sshpass");
        _exit(127);
    }

    proxyChildPid = child;
    ::close(passwordPipe[0]);
    const bool passwordWritten = writeExact(passwordPipe[1], passwordLine);
    OPENSSL_cleanse(passwordLine.data(), passwordLine.size());
    ::close(passwordPipe[1]);
    if (!passwordWritten) {
        ::kill(child, SIGTERM);
    }

    int status = 0;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return reportError("Could not wait for sshpass");
        }
    }
    proxyChildPid = -1;
    if (!passwordWritten) {
        return reportError("Could not pass the SSH password to sshpass");
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}
