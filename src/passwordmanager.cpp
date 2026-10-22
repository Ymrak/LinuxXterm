#include "passwordmanager.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QDebug>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {
constexpr int KdfIterations = 600000;
constexpr int SaltSize = 16;
constexpr int KeySize = 32;
constexpr int NonceSize = 12;
constexpr int TagSize = 16;

QByteArray legacyHash(const QString &password)
{
    return QCryptographicHash::hash(
        (password + "LinuxXtermSalt2026").toUtf8(),
        QCryptographicHash::Sha256
    ).toBase64();
}

bool deriveKey(const QString &password, const QByteArray &salt, QByteArray *key)
{
    QByteArray passwordBytes = password.toUtf8();
    key->resize(KeySize);
    const bool derived = PKCS5_PBKDF2_HMAC(
        passwordBytes.constData(),
        passwordBytes.size(),
        reinterpret_cast<const unsigned char *>(salt.constData()),
        salt.size(),
        KdfIterations,
        EVP_sha256(),
        key->size(),
        reinterpret_cast<unsigned char *>(key->data())
    ) == 1;
    OPENSSL_cleanse(passwordBytes.data(), passwordBytes.size());
    return derived;
}

bool writePrivateFile(QSaveFile &file, const QByteArray &data)
{
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}
}

PasswordManager::PasswordManager(QObject *parent)
    : QObject(parent)
    , m_isUnlocked(false)
{
    const QString configDir = QDir::homePath() + "/.config/linuxxterm";
    QDir().mkpath(configDir);
    QFile::setPermissions(configDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                      | QFileDevice::ExeOwner);

    m_vaultPath = configDir + "/password_vault.dat";
    m_hashPath = configDir + "/master_password.hash";

    QFile hashFile(m_hashPath);
    if (hashFile.open(QIODevice::ReadOnly)) {
        m_masterPasswordHash = hashFile.readAll();
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
    if (hasMasterPassword() || password.isEmpty()) {
        return false;
    }

    m_vaultSalt.resize(SaltSize);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(m_vaultSalt.data()), m_vaultSalt.size()) != 1
        || !deriveKey(password, m_vaultSalt, &m_encryptionKey)) {
        m_vaultSalt.clear();
        m_encryptionKey.clear();
        return false;
    }

    m_isUnlocked = true;
    if (!saveVault() || !saveMasterPasswordHash(password)) {
        lock();
        return false;
    }

    emit vaultUnlocked();
    return true;
}

bool PasswordManager::verifyMasterPassword(const QString &password)
{
    const QJsonDocument document = QJsonDocument::fromJson(m_masterPasswordHash);
    if (!document.isObject() || document.object().value("version").toInt() != 2) {
        const QByteArray expected = m_masterPasswordHash.trimmed();
        const QByteArray actual = legacyHash(password);
        return actual.size() == expected.size()
            && CRYPTO_memcmp(actual.constData(), expected.constData(), expected.size()) == 0;
    }

    const QJsonObject hashRecord = document.object();
    const QByteArray salt = QByteArray::fromBase64(hashRecord.value("salt").toString().toLatin1());
    const QByteArray expected = QByteArray::fromBase64(hashRecord.value("hash").toString().toLatin1());
    QByteArray actual;
    if (salt.size() != SaltSize || expected.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)
        || !deriveKey(password, salt, &actual)) {
        return false;
    }

    const bool matches = CRYPTO_memcmp(actual.constData(), expected.constData(), expected.size()) == 0;
    OPENSSL_cleanse(actual.data(), actual.size());
    return matches;
}

bool PasswordManager::unlock(const QString &password)
{
    if (!hasMasterPassword()) {
        return true;
    }
    if (password.isEmpty() || !verifyMasterPassword(password)) {
        return false;
    }

    const QJsonDocument hashDocument = QJsonDocument::fromJson(m_masterPasswordHash);
    const bool legacyHashFormat =
        !hashDocument.isObject() || hashDocument.object().value("version").toInt() != 2;

    if (!loadVault(password)) {
        lock();
        return false;
    }

    m_isUnlocked = true;
    if (!saveVault()
        || (legacyHashFormat && !saveMasterPasswordHash(password))) {
        lock();
        return false;
    }

    emit vaultUnlocked();
    return true;
}

void PasswordManager::lock()
{
    if (!m_encryptionKey.isEmpty()) {
        OPENSSL_cleanse(m_encryptionKey.data(), m_encryptionKey.size());
    }
    m_encryptionKey.clear();
    m_vaultSalt.clear();
    for (QString &password : m_passwords) {
        password.fill(QChar());
    }
    m_passwords.clear();
    if (m_isUnlocked) {
        m_isUnlocked = false;
        emit vaultLocked();
    }
}

bool PasswordManager::isUnlocked() const
{
    return m_isUnlocked;
}

bool PasswordManager::savePassword(const QString &connectionId, const QString &password)
{
    QMap<QString, QString> passwords;
    passwords.insert(connectionId, password);
    return savePasswords(passwords);
}

bool PasswordManager::savePasswords(const QMap<QString, QString> &passwords)
{
    if (!m_isUnlocked) {
        return false;
    }

    const QMap<QString, QString> previous = m_passwords;
    for (auto it = passwords.cbegin(); it != passwords.cend(); ++it) {
        m_passwords[it.key()] = it.value();
    }
    if (!saveVault()) {
        m_passwords = previous;
        return false;
    }
    return true;
}

QString PasswordManager::getPassword(const QString &connectionId) const
{
    if (!m_isUnlocked) {
        return QString();
    }
    return m_passwords.value(connectionId);
}

void PasswordManager::removePassword(const QString &connectionId)
{
    if (!m_isUnlocked || !m_passwords.contains(connectionId)) {
        return;
    }

    const QString previous = m_passwords.take(connectionId);
    if (!saveVault()) {
        m_passwords.insert(connectionId, previous);
        qWarning() << "Не удалось сохранить удаление пароля из хранилища";
    }
}

QStringList PasswordManager::getConnectionIds() const
{
    return m_passwords.keys();
}

bool PasswordManager::saveMasterPasswordHash(const QString &password)
{
    QByteArray salt(SaltSize, Qt::Uninitialized);
    QByteArray hash;
    if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), salt.size()) != 1
        || !deriveKey(password, salt, &hash)) {
        return false;
    }

    QJsonObject record;
    record["version"] = 2;
    record["salt"] = QString::fromLatin1(salt.toBase64());
    record["hash"] = QString::fromLatin1(hash.toBase64());
    OPENSSL_cleanse(hash.data(), hash.size());
    QSaveFile file(m_hashPath);
    if (!writePrivateFile(file, QJsonDocument(record).toJson(QJsonDocument::Compact))) {
        return false;
    }

    m_masterPasswordHash = QJsonDocument(record).toJson(QJsonDocument::Compact);
    return true;
}

bool PasswordManager::encryptPassword(const QString &connectionId, const QString &password,
                                      QJsonObject *record) const
{
    QByteArray nonce(NonceSize, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size()) != 1) {
        return false;
    }

    const QByteArray plaintext = password.toUtf8();
    QByteArray ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    QByteArray tag(TagSize, Qt::Uninitialized);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (!context) {
        return false;
    }

    int outputLength = 0;
    int finalLength = 0;
    int associatedDataLength = 0;
    const QByteArray associatedData = connectionId.toUtf8();
    const bool encrypted =
        EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1
        && EVP_EncryptInit_ex(
            context,
            nullptr,
            nullptr,
            reinterpret_cast<const unsigned char *>(m_encryptionKey.constData()),
            reinterpret_cast<const unsigned char *>(nonce.constData())
        ) == 1
        && EVP_EncryptUpdate(
            context,
            nullptr,
            &associatedDataLength,
            reinterpret_cast<const unsigned char *>(associatedData.constData()),
            associatedData.size()
        ) == 1
        && EVP_EncryptUpdate(
            context,
            reinterpret_cast<unsigned char *>(ciphertext.data()),
            &outputLength,
            reinterpret_cast<const unsigned char *>(plaintext.constData()),
            plaintext.size()
        ) == 1
        && EVP_EncryptFinal_ex(
            context,
            reinterpret_cast<unsigned char *>(ciphertext.data()) + outputLength,
            &finalLength
        ) == 1
        && EVP_CIPHER_CTX_ctrl(
            context,
            EVP_CTRL_GCM_GET_TAG,
            tag.size(),
            tag.data()
        ) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!encrypted) {
        return false;
    }

    ciphertext.resize(outputLength + finalLength);
    (*record)["nonce"] = QString::fromLatin1(nonce.toBase64());
    (*record)["ciphertext"] = QString::fromLatin1(ciphertext.toBase64());
    (*record)["tag"] = QString::fromLatin1(tag.toBase64());
    return true;
}

bool PasswordManager::decryptPassword(const QString &connectionId, const QJsonObject &record,
                                      QString *password) const
{
    const QByteArray nonce = QByteArray::fromBase64(record.value("nonce").toString().toLatin1());
    const QByteArray ciphertext =
        QByteArray::fromBase64(record.value("ciphertext").toString().toLatin1());
    QByteArray tag = QByteArray::fromBase64(record.value("tag").toString().toLatin1());
    if (nonce.size() != NonceSize || tag.size() != TagSize || m_encryptionKey.size() != KeySize) {
        return false;
    }

    QByteArray plaintext(ciphertext.size() + EVP_MAX_BLOCK_LENGTH, Qt::Uninitialized);
    EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
    if (!context) {
        return false;
    }

    int outputLength = 0;
    int finalLength = 0;
    int associatedDataLength = 0;
    const QByteArray associatedData = connectionId.toUtf8();
    const bool initialized =
        EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1
        && EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1
        && EVP_DecryptInit_ex(
            context,
            nullptr,
            nullptr,
            reinterpret_cast<const unsigned char *>(m_encryptionKey.constData()),
            reinterpret_cast<const unsigned char *>(nonce.constData())
        ) == 1
        && EVP_DecryptUpdate(
            context,
            nullptr,
            &associatedDataLength,
            reinterpret_cast<const unsigned char *>(associatedData.constData()),
            associatedData.size()
        ) == 1
        && EVP_DecryptUpdate(
            context,
            reinterpret_cast<unsigned char *>(plaintext.data()),
            &outputLength,
            reinterpret_cast<const unsigned char *>(ciphertext.constData()),
            ciphertext.size()
        ) == 1
        && EVP_CIPHER_CTX_ctrl(
            context,
            EVP_CTRL_GCM_SET_TAG,
            tag.size(),
            tag.data()
        ) == 1;
    const bool authenticated = initialized
        && EVP_DecryptFinal_ex(
            context,
            reinterpret_cast<unsigned char *>(plaintext.data()) + outputLength,
            &finalLength
        ) == 1;
    EVP_CIPHER_CTX_free(context);
    if (!authenticated) {
        return false;
    }

    plaintext.resize(outputLength + finalLength);
    *password = QString::fromUtf8(plaintext);
    return true;
}

bool PasswordManager::saveVault()
{
    if (!m_isUnlocked || m_encryptionKey.size() != KeySize) {
        return false;
    }

    QJsonObject passwords;
    for (auto it = m_passwords.cbegin(); it != m_passwords.cend(); ++it) {
        QJsonObject record;
        if (!encryptPassword(it.key(), it.value(), &record)) {
            return false;
        }
        passwords[it.key()] = record;
    }

    QJsonObject vault;
    vault["version"] = 2;
    vault["salt"] = QString::fromLatin1(m_vaultSalt.toBase64());
    vault["passwords"] = passwords;

    QSaveFile file(m_vaultPath);
    return writePrivateFile(file, QJsonDocument(vault).toJson(QJsonDocument::Compact));
}

bool PasswordManager::loadVault(const QString &password)
{
    QFile vaultFile(m_vaultPath);
    if (!vaultFile.exists()) {
        m_vaultSalt.resize(SaltSize);
        return RAND_bytes(
            reinterpret_cast<unsigned char *>(m_vaultSalt.data()),
            m_vaultSalt.size()
        ) == 1 && deriveKey(password, m_vaultSalt, &m_encryptionKey);
    }
    if (!vaultFile.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QJsonDocument document = QJsonDocument::fromJson(vaultFile.readAll());
    if (!document.isObject()) {
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value("version").toInt() == 2) {
        m_vaultSalt = QByteArray::fromBase64(root.value("salt").toString().toLatin1());
        if (m_vaultSalt.size() != SaltSize
            || !deriveKey(password, m_vaultSalt, &m_encryptionKey)
            || !root.value("passwords").isObject()) {
            return false;
        }

        QMap<QString, QString> loadedPasswords;
        const QJsonObject passwords = root.value("passwords").toObject();
        for (auto it = passwords.begin(); it != passwords.end(); ++it) {
            QString plaintext;
            if (!it.value().isObject()
                || !decryptPassword(it.key(), it.value().toObject(), &plaintext)) {
                return false;
            }
            loadedPasswords.insert(it.key(), plaintext);
        }
        m_passwords = loadedPasswords;
        return true;
    }

    m_vaultSalt.resize(SaltSize);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(m_vaultSalt.data()), m_vaultSalt.size()) != 1
        || !deriveKey(password, m_vaultSalt, &m_encryptionKey)) {
        return false;
    }

    QMap<QString, QString> loadedPasswords;
    const QJsonObject legacyVault = document.object();
    const QByteArray legacyKey = password.toUtf8();
    for (auto it = legacyVault.begin(); it != legacyVault.end(); ++it) {
        if (!it.value().isString()) {
            return false;
        }
        QByteArray data = QByteArray::fromBase64(it.value().toString().toLatin1());
        for (int i = 0; i < data.size(); ++i) {
            data[i] = data[i] ^ legacyKey[i % legacyKey.size()];
        }
        loadedPasswords.insert(it.key(), QString::fromUtf8(data));
    }
    m_passwords = loadedPasswords;
    return true;
}
