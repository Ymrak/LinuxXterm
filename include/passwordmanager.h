#ifndef PASSWORDMANAGER_H
#define PASSWORDMANAGER_H

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QMap>

class PasswordManager : public QObject
{
    Q_OBJECT

public:
    explicit PasswordManager(QObject *parent = nullptr);
    ~PasswordManager();

    bool hasMasterPassword() const;
    
    bool setMasterPassword(const QString &password);
    
    bool verifyMasterPassword(const QString &password);
    
    bool unlock(const QString &password);
    
    void lock();
    
    bool isUnlocked() const;

    bool savePassword(const QString &connectionId, const QString &password);
    bool savePasswords(const QMap<QString, QString> &passwords);
    
    QString getPassword(const QString &connectionId) const;
    
    void removePassword(const QString &connectionId);
    
    QStringList getConnectionIds() const;

signals:
    void vaultLocked();
    void vaultUnlocked();

private:
    bool encryptPassword(const QString &connectionId, const QString &password,
                         QJsonObject *record) const;
    bool decryptPassword(const QString &connectionId, const QJsonObject &record,
                         QString *password) const;
    bool saveMasterPasswordHash(const QString &password);
    bool saveVault();
    bool loadVault(const QString &password);

    QByteArray m_masterPasswordHash;
    bool m_isUnlocked;
    QMap<QString, QString> m_passwords;
    QByteArray m_encryptionKey;
    QByteArray m_vaultSalt;
    QString m_vaultPath;
    QString m_hashPath;
};

#endif