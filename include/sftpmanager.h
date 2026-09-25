#ifndef SFTPMANAGER_H
#define SFTPMANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <libssh2.h>
#include <libssh2_sftp.h>

// Структура для хранения информации о файле/папке
struct SftpFileInfo {
    QString name;
    QString path;
    qint64 size;
    bool isDir;
    QDateTime modifiedTime;
};

class SftpManager : public QObject
{
    Q_OBJECT

public:
    explicit SftpManager(QObject *parent = nullptr);
    ~SftpManager();

    // Подключение к SFTP через существующую libssh2 сессию или напрямую
    bool connectToHost(const QString &host, int port, const QString &user, 
                       const QString &password, const QString &privateKeyPath = QString());
    
    void disconnect();
    bool isConnected() const { return m_isConnected; }

    // Получить домашнюю директорию пользователя через SSH
    QString getHomeDirectory();

    // Операции с файлами
    QList<SftpFileInfo> listDirectory(const QString &remotePath);
    bool downloadFile(const QString &remotePath, const QString &localPath);
    bool uploadFile(const QString &localPath, const QString &remotePath);
    bool deleteFile(const QString &remotePath);
    bool createDirectory(const QString &remotePath);

    // Получение последней ошибки
    QString lastError() const { return m_lastError; }

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void directoryListed(const QString &path, const QList<SftpFileInfo> &files);

private:
    LIBSSH2_SESSION *m_session;
    LIBSSH2_SFTP *m_sftp;
    int m_socket;
    bool m_isConnected;
    QString m_lastError;

    // Внутренний метод для установки ошибки
    void setError(const QString &error);
};

#endif // SFTPMANAGER_H