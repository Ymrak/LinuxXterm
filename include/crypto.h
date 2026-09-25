#ifndef CRYPTO_H
#define CRYPTO_H

#include <QString>

class Crypto
{
public:
    // Шифрование строки
    static QString encrypt(const QString &plainText);
    
    // Расшифровка строки
    static QString decrypt(const QString &cipherText);

private:
    // Ключ шифрования (можно изменить на любой свой)
    static const QString KEY;
};

#endif // CRYPTO_H
