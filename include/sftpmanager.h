#ifndef SFTPMANAGER_H
#define SFTPMANAGER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QByteArray>
#include <libssh2.h>
#include <libssh2_sftp.h>

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

    bool connectToHost(const QString &host, int port, const QString &user, 
                       const QString &password, const QString &privateKeyPath = QString(),
                       bool allowLegacyHostKeys = false);
    
    void disconnect();
    bool isConnected() const { return m_isConnected; }

    QString getHomeDirectory();

    QString executeCommand(const QString &command);

    QList<SftpFileInfo> listDirectory(const QString &remotePath);
    bool downloadFile(const QString &remotePath, const QString &localPath);
    bool uploadFile(const QString &localPath, const QString &remotePath);
    bool deleteFile(const QString &remotePath);
    bool createDirectory(const QString &remotePath);

    QString lastError() const { return m_lastError; }

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void directoryListed(const QString &path, const QList<SftpFileInfo> &files);
    
    void uploadProgress(const QString &fileName, qint64 bytesSent, qint64 bytesTotal);
    void downloadProgress(const QString &fileName, qint64 bytesReceived, qint64 bytesTotal);
    void operationFinished(const QString &fileName, bool success);

private:
    LIBSSH2_SESSION *m_session;
    LIBSSH2_SFTP *m_sftp;
    int m_socket;
    bool m_isConnected;
    QString m_lastError;
    bool m_legacyMode = false;
    QString m_host;
    int m_port = 22;
    QString m_user;
    QString m_password;
    QString m_privateKeyPath;
    bool m_allowLegacyHostKeys = false;
    QString m_homeDirectory;

    void setError(const QString &error);
    bool runLegacySftpBatch(const QStringList &commands, QByteArray *standardOutput = nullptr,
                            const QString &progressFile = QString(),
                            const QString &localProgressPath = QString(),
                            qint64 progressTotal = -1);
    QString runLegacySshCommand(const QString &command);
    static QString quoteSftpArgument(const QString &argument);
};

#endif