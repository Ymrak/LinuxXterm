#include "legacyssh.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

QString legacySshOpenSslConfigPath(QString *errorMessage)
{
    if (errorMessage) {
        errorMessage->clear();
    }

    QFile systemPolicy("/etc/crypto-policies/back-ends/opensslcnf.config");
    if (!systemPolicy.open(QIODevice::ReadOnly) ||
        !systemPolicy.readAll().contains("rh-allow-sha1-signatures")) {
        return {};
    }

    const QString configDirectory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configDirectory.isEmpty() || !QDir().mkpath(configDirectory)) {
        if (errorMessage) {
            *errorMessage = "Не удалось создать каталог конфигурации LinuxXterm";
        }
        return {};
    }

    const QString configPath = configDirectory + "/legacy-openssl.cnf";
    const QByteArray configContents =
        "openssl_conf = openssl_init\n"
        "[openssl_init]\n"
        "alg_section = evp_properties\n"
        "[evp_properties]\n"
        "rh-allow-sha1-signatures = yes\n";

    QFile configFile(configPath);
    if (configFile.exists()) {
        if (configFile.open(QIODevice::ReadOnly)) {
            const QByteArray existingContents = configFile.readAll();
            configFile.close();
            if (existingContents == configContents) {
                return configPath;
            }
        }
    }

    if (!configFile.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        configFile.write(configContents) != configContents.size() ||
        !configFile.flush()) {
        if (errorMessage) {
            *errorMessage = "Не удалось записать изолированную конфигурацию OpenSSL";
        }
        return {};
    }
    configFile.close();
    if (!configFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (errorMessage) {
            *errorMessage = "Не удалось ограничить права доступа к конфигурации OpenSSL";
        }
        return {};
    }

    return configPath;
}

QStringList legacySshOptions()
{
    return {
        "-o", "HostKeyAlgorithms=+ssh-rsa",
        "-o", "PubkeyAcceptedAlgorithms=+ssh-rsa",
        "-o", "KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1",
        "-o", "Ciphers=+aes128-cbc,aes192-cbc,aes256-cbc,3des-cbc",
        "-o", "MACs=+hmac-sha1"
    };
}
