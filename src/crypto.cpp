#include "crypto.h"
#include <QByteArray>

// Ключ шифрования
const QString Crypto::KEY = "LinuxXterm_Secret_Key_2026";

QString Crypto::encrypt(const QString &plainText)
{
    if (plainText.isEmpty()) {
        return plainText;
    }

    QByteArray bytes = plainText.toUtf8();
    QByteArray keyBytes = KEY.toUtf8();
    int keyLength = keyBytes.size();

    // XOR шифрование
    for (int i = 0; i < bytes.size(); ++i) {
        bytes[i] = bytes[i] ^ keyBytes[i % keyLength];
    }

    // Кодируем в Base64, чтобы можно было безопасно сохранить в JSON
    return QString::fromLatin1(bytes.toBase64());
}

QString Crypto::decrypt(const QString &cipherText)
{
    if (cipherText.isEmpty()) {
        return cipherText;
    }

    // Декодируем из Base64
    QByteArray bytes = QByteArray::fromBase64(cipherText.toLatin1());
    QByteArray keyBytes = KEY.toUtf8();
    int keyLength = keyBytes.size();

    // XOR расшифровка (та же операция, что и шифрование)
    for (int i = 0; i < bytes.size(); ++i) {
        bytes[i] = bytes[i] ^ keyBytes[i % keyLength];
    }

    return QString::fromUtf8(bytes);
}
