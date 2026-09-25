#include "passwordmanager.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QTextStream>

PasswordManager::PasswordManager(QObject *parent)
    : QObject(parent)
    , m_isUnlocked(false)  // ← ВАЖНО: всегда false при старте
{
    QString configDir = QDir::homePath() + "/.config/linuxxterm";
    QDir().mkpath(configDir);
    
    m_vaultPath = configDir + "/password_vault.dat";
    m_hashPath = configDir + "/master_password.hash";
    
    // Загрузка хеша мастер-пароля
    QFile hashFile(m_hashPath);
    if (hashFile.exists()) {
        if (hashFile.open(QIODevice::ReadOnly)) {
            m_masterPasswordHash = QString::fromUtf8(hashFile.readAll());
            hashFile.close();
        }
    }
}

PasswordManager::~PasswordManager()
{
    lock();
}

bool PasswordManager::hasMasterPassword() const
{
    return !m_masterPasswordHash.isEmpty();
}

bool PasswordManager::setMasterPassword(const QString &password)
{
    if (hasMasterPassword()) {
        return false;
    }

    m_masterPasswordHash = hashPassword(password);
    m_masterPassword = password;
    m_isUnlocked = true;  // ← КРИТИЧНО: устанавливаем флаг разблокировки

    // Сохраняем хеш
    QFile hashFile(m_hashPath);
    if (hashFile.open(QIODevice::WriteOnly)) {
        hashFile.write(m_masterPasswordHash.toUtf8());
        hashFile.close();
        
        loadVault();  // Загружаем хранилище
        
        emit vaultUnlocked();
        return true;
    }

    return false;
}

bool PasswordManager::verifyMasterPassword(const QString &password)
{
    return hashPassword(password) == m_masterPasswordHash;
}

bool PasswordManager::unlock(const QString &password)
{
    if (!hasMasterPassword()) {
        return true;
    }

    if (verifyMasterPassword(password)) {
        m_masterPassword = password;
        m_isUnlocked = true;  // ← КРИТИЧНО: устанавливаем флаг разблокировки
        
        if (loadVault()) {
            emit vaultUnlocked();
            return true;
        }
    }

    return false;
}

void PasswordManager::lock()
{
    if (m_isUnlocked) {
        m_isUnlocked = false;
        m_masterPassword.clear();
        m_passwords.clear();
        emit vaultLocked();
    }
}

bool PasswordManager::isUnlocked() const
{
    return m_isUnlocked;  // ← ИСПРАВЛЕНО: просто возвращаем флаг
}

void PasswordManager::savePassword(const QString &connectionId, const QString &password)
{
    if (!m_isUnlocked) {  // ← ИСПРАВЛЕНО: проверяем флаг
        return;
    }

    m_passwords[connectionId] = encryptPassword(password);
    saveVault();
}

QString PasswordManager::getPassword(const QString &connectionId) const
{
    if (!m_isUnlocked) {  // ← ИСПРАВЛЕНО: проверяем флаг
        return QString();
    }

    QString encrypted = m_passwords.value(connectionId);
    if (encrypted.isEmpty()) {
        return QString();
    }

    return decryptPassword(encrypted);
}

void PasswordManager::removePassword(const QString &connectionId)
{
    if (m_passwords.contains(connectionId)) {
        m_passwords.remove(connectionId);
        saveVault();
    }
}

QStringList PasswordManager::getConnectionIds() const
{
    return m_passwords.keys();
}

QString PasswordManager::hashPassword(const QString &password) const
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            (password + "LinuxXtermSalt2026").toUtf8(),
            QCryptographicHash::Sha256
        ).toBase64()
    );
}

QString PasswordManager::encryptPassword(const QString &password) const
{
    if (m_masterPassword.isEmpty()) {
        return password;
    }

    QByteArray data = password.toUtf8();
    QByteArray key = m_masterPassword.toUtf8();
    
    for (int i = 0; i < data.size(); ++i) {
        data[i] = data[i] ^ key[i % key.size()];
    }

    return QString::fromLatin1(data.toBase64());
}

QString PasswordManager::decryptPassword(const QString &encryptedPassword) const
{
    if (m_masterPassword.isEmpty()) {
        return encryptedPassword;
    }

    QByteArray data = QByteArray::fromBase64(encryptedPassword.toLatin1());
    QByteArray key = m_masterPassword.toUtf8();
    
    for (int i = 0; i < data.size(); ++i) {
        data[i] = data[i] ^ key[i % key.size()];
    }

    return QString::fromUtf8(data);
}

void PasswordManager::saveVault()
{
    if (!m_isUnlocked) {
        return;
    }

    QJsonObject vault;
    for (auto it = m_passwords.begin(); it != m_passwords.end(); ++it) {
        vault[it.key()] = it.value();
    }

    QJsonDocument doc(vault);
    QFile vaultFile(m_vaultPath);
    if (vaultFile.open(QIODevice::WriteOnly)) {
        vaultFile.write(doc.toJson());
        vaultFile.close();
    }
}

bool PasswordManager::loadVault()
{
    QFile vaultFile(m_vaultPath);
    if (!vaultFile.exists()) {
        return true;
    }

    if (!vaultFile.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray data = vaultFile.readAll();
    vaultFile.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        return false;
    }

    QJsonObject vault = doc.object();
    m_passwords.clear();
    
    for (auto it = vault.begin(); it != vault.end(); ++it) {
        m_passwords[it.key()] = it.value().toString();
    }

    return true;
}