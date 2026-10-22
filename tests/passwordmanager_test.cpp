#include "connectionmanager.h"
#include "passwordmanager.h"
#include "sshpassproxy.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QDebug>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace {
bool require(bool condition, const char *message)
{
    if (!condition) {
        qCritical() << message;
    }
    return condition;
}

bool prepareHome(QTemporaryDir *home)
{
    if (!home->isValid()) {
        return false;
    }
    qputenv("HOME", home->path().toLocal8Bit());
    return QDir().mkpath(home->path() + "/.config/linuxxterm");
}

bool testEncryptedRoundTrip()
{
    QTemporaryDir home;
    if (!require(prepareHome(&home), "Temporary home could not be prepared")) {
        return false;
    }

    PasswordManager manager;
    const QString secret = "round-trip-password";
    if (!require(manager.setMasterPassword("correct horse"), "Master password setup failed")
        || !require(manager.savePassword("connection-a", secret), "Password save failed")
        || !require(manager.savePassword("connection-b", "second-password"),
                    "Second password save failed")) {
        return false;
    }

    manager.lock();
    const QString vaultPath = home.path() + "/.config/linuxxterm/password_vault.dat";
    QFile vault(vaultPath);
    if (!require(vault.open(QIODevice::ReadOnly), "Vault file could not be read")) {
        return false;
    }
    const QByteArray contents = vault.readAll();
    if (!require(!contents.contains(secret.toUtf8()), "Vault contains a plaintext password")) {
        return false;
    }
    const QFileDevice::Permissions sensitivePermissions =
        QFileInfo(vaultPath).permissions()
        & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
           | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther);
    if (!require(sensitivePermissions == QFileDevice::Permissions(),
                 "Vault permissions are accessible to other users")) {
        return false;
    }

    if (!require(!manager.unlock("wrong password"), "Incorrect master password was accepted")
        || !require(manager.unlock("correct horse"), "Correct master password was rejected")
        || !require(manager.getPassword("connection-a") == secret, "Password round trip failed")
        || !require(manager.getPassword("connection-b") == "second-password",
                    "Second password round trip failed")) {
        return false;
    }

    manager.lock();
    QFile tamperedVault(vaultPath);
    if (!require(tamperedVault.open(QIODevice::ReadOnly), "Vault could not be read for tampering test")) {
        return false;
    }
    QJsonObject vaultObject = QJsonDocument::fromJson(tamperedVault.readAll()).object();
    tamperedVault.close();
    QJsonObject passwordRecords = vaultObject.value("passwords").toObject();
    const QJsonValue firstRecord = passwordRecords.value("connection-a");
    passwordRecords["connection-a"] = passwordRecords.value("connection-b");
    passwordRecords["connection-b"] = firstRecord;
    vaultObject["passwords"] = passwordRecords;
    if (!require(tamperedVault.open(QIODevice::WriteOnly | QIODevice::Truncate),
                 "Tampered vault could not be written")) {
        return false;
    }
    tamperedVault.write(QJsonDocument(vaultObject).toJson());
    tamperedVault.close();
    if (!require(!manager.unlock("correct horse"), "Vault accepted ciphertext bound to another ID")) {
        return false;
    }
    return true;
}

bool testLegacyMigration()
{
    QTemporaryDir home;
    if (!require(prepareHome(&home), "Temporary home could not be prepared")) {
        return false;
    }

    const QString masterPassword = "existing master password";
    const QByteArray oldHash = QCryptographicHash::hash(
        (masterPassword + "LinuxXtermSalt2026").toUtf8(),
        QCryptographicHash::Sha256
    ).toBase64();
    const QByteArray legacyVaultPassword = "legacy-vault-password";
    QByteArray encryptedLegacyPassword = legacyVaultPassword;
    const QByteArray xorKey = masterPassword.toUtf8();
    for (int i = 0; i < encryptedLegacyPassword.size(); ++i) {
        encryptedLegacyPassword[i] = encryptedLegacyPassword[i] ^ xorKey[i % xorKey.size()];
    }

    QFile hashFile(home.path() + "/.config/linuxxterm/master_password.hash");
    QFile vaultFile(home.path() + "/.config/linuxxterm/password_vault.dat");
    QFile connectionsFile(home.path() + "/.config/linuxxterm/connections.json");
    if (!require(hashFile.open(QIODevice::WriteOnly), "Legacy hash file could not be created")) {
        return false;
    }
    hashFile.write(oldHash);
    hashFile.close();

    QJsonObject oldVault;
    oldVault["legacy-vault-id"] = QString::fromLatin1(encryptedLegacyPassword.toBase64());
    if (!require(vaultFile.open(QIODevice::WriteOnly), "Legacy vault file could not be created")) {
        return false;
    }
    vaultFile.write(QJsonDocument(oldVault).toJson());
    vaultFile.close();

    QJsonObject legacyConnection;
    legacyConnection["id"] = "legacy-profile-id";
    legacyConnection["name"] = "Legacy profile";
    legacyConnection["host"] = "example.test";
    legacyConnection["port"] = 22;
    legacyConnection["password"] = "legacy-profile-password";
    QJsonArray profiles;
    profiles.append(legacyConnection);
    if (!require(connectionsFile.open(QIODevice::WriteOnly), "Legacy profiles file could not be created")) {
        return false;
    }
    connectionsFile.write(QJsonDocument(profiles).toJson());
    connectionsFile.close();

    {
        ConnectionManager connections;
        PasswordManager passwords;
        if (!require(passwords.unlock(masterPassword), "Existing master password was rejected")
            || !require(passwords.getPassword("legacy-vault-id") == legacyVaultPassword,
                        "Legacy vault password was not preserved")) {
            return false;
        }

        QMap<QString, QString> legacyPasswords;
        for (const Connection &connection : connections.getConnections()) {
            if (!connection.getPassword().isEmpty()) {
                legacyPasswords.insert(connection.getId(), connection.getPassword());
            }
        }
        if (!require(passwords.savePasswords(legacyPasswords), "Profile passwords were not encrypted")
            || !require(connections.clearPlaintextPasswords(), "Plaintext profile passwords were not removed")
            || !require(passwords.getPassword("legacy-profile-id") == "legacy-profile-password",
                        "Migrated profile password was not preserved")) {
            return false;
        }

        QFile migratedVault(home.path() + "/.config/linuxxterm/password_vault.dat");
        if (!require(migratedVault.open(QIODevice::ReadOnly), "Migrated vault could not be read")) {
            return false;
        }
        const QByteArray migratedVaultContents = migratedVault.readAll();
        if (!require(!migratedVaultContents.contains(legacyVaultPassword)
                     && !migratedVaultContents.contains("legacy-profile-password"),
                     "Migrated vault contains plaintext")) {
            return false;
        }

        QFile migratedConnections(home.path() + "/.config/linuxxterm/connections.json");
        if (!require(migratedConnections.open(QIODevice::ReadOnly), "Migrated profiles could not be read")) {
            return false;
        }
        const QJsonArray savedProfiles =
            QJsonDocument::fromJson(migratedConnections.readAll()).array();
        if (!require(savedProfiles.size() == 1
                     && !savedProfiles.first().toObject().contains("password"),
                     "Migrated profiles still contain a password field")) {
            return false;
        }
        QFile migratedHash(home.path() + "/.config/linuxxterm/master_password.hash");
        if (!require(migratedHash.open(QIODevice::ReadOnly), "Migrated master hash could not be read")) {
            return false;
        }
        const QJsonObject hashRecord = QJsonDocument::fromJson(migratedHash.readAll()).object();
        if (!require(hashRecord.value("version").toInt() == 2,
                     "Legacy master hash was not upgraded")) {
            return false;
        }
        passwords.lock();
    }

    PasswordManager reopened;
    return require(reopened.unlock(masterPassword), "Migrated master password was rejected")
        && require(reopened.getPassword("legacy-vault-id") == legacyVaultPassword,
                   "Migrated legacy vault password could not be reopened")
        && require(reopened.getPassword("legacy-profile-id") == "legacy-profile-password",
                   "Migrated profile password could not be reopened");
}

int runFakeSshpass(int argc, char *argv[])
{
    if (argc < 4 || std::strcmp(argv[1], "-d") != 0) {
        return 2;
    }
    const int passwordDescriptor = std::atoi(argv[2]);
    QByteArray password;
    char character = 0;
    while (::read(passwordDescriptor, &character, 1) == 1 && character != '\n') {
        password.append(character);
    }

    QByteArray input;
    char buffer[256];
    ssize_t count = 0;
    while ((count = ::read(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        input.append(buffer, count);
    }

    const QByteArray output = "password=" + password + "\nenvironment="
        + qgetenv("SSHPASS") + "\ninput=" + input;
    ::write(STDOUT_FILENO, output.constData(), output.size());
    return 0;
}

bool testSshpassProxy()
{
    const QString password = "proxy-password";
    const QString socketName = createSshpassProxySocketName();
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!require(server.listen(socketName), "Password proxy socket could not be created")) {
        return false;
    }

    QLocalSocket attacker;
    attacker.connectToServer(socketName);
    if (!require(attacker.waitForConnected(5000), "Unauthorized socket test client did not connect")) {
        return false;
    }

    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SSHPASS", password);
    process.setProcessEnvironment(environment);
    process.start(
        QCoreApplication::applicationFilePath(),
        makeSshpassProxyArguments(
            socketName,
            QCoreApplication::applicationFilePath(),
            {"--fake-sshpass"}
        )
    );
    if (!require(process.waitForStarted(5000), "Password proxy process did not start")) {
        process.kill();
        process.waitForFinished();
        QLocalServer::removeServer(socketName);
        return false;
    }

    QLocalSocket *client = waitForSshpassProxy(&server, process.processId(), 5000);
    if (!require(client != nullptr, "Password proxy did not authenticate its local socket")) {
        process.kill();
        process.waitForFinished();
        QLocalServer::removeServer(socketName);
        return false;
    }
    if (!require(sendSshpassProxyPassword(client, password),
                 "Password proxy could not send the password")) {
        process.kill();
        process.waitForFinished();
        QLocalServer::removeServer(socketName);
        return false;
    }
    client->disconnectFromServer();
    server.close();
    QLocalServer::removeServer(socketName);

    process.write("sftp-command\n");
    process.closeWriteChannel();
    if (!require(process.waitForFinished(5000), "Password proxy process did not finish")) {
        process.kill();
        process.waitForFinished();
        return false;
    }
    const QByteArray output = process.readAllStandardOutput();
    return require(process.exitCode() == 0, "Password proxy returned an error")
        && require(output.contains("password=" + password.toUtf8()),
                   "Password proxy did not send the password through its pipe")
        && require(output.contains("environment=\n"),
                   "Password proxy left SSHPASS in the environment")
        && require(output.contains("input=sftp-command\n"),
                   "Password proxy did not preserve the process input stream");
}
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--sshpass-proxy") == 0) {
        return runSshpassProxy(argc, argv);
    }
    if (argc > 1 && std::strcmp(argv[1], "-d") == 0) {
        return runFakeSshpass(argc, argv);
    }
    return testEncryptedRoundTrip() && testLegacyMigration() && testSshpassProxy() ? 0 : 1;
}
