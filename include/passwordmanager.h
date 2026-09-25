#ifndef PASSWORDMANAGER_H
#define PASSWORDMANAGER_H

#include <QObject>
#include <QString>
#include <QMap>

class PasswordManager : public QObject
{
    Q_OBJECT

public:
    explicit PasswordManager(QObject *parent = nullptr);
    ~PasswordManager();

    // Проверка наличия мастер-пароля (установлен ли он)
    bool hasMasterPassword() const;
    
    // Установка мастер-пароля (первый раз)
    bool setMasterPassword(const QString &password);
    
    // Проверка мастер-пароля
    bool verifyMasterPassword(const QString &password);
    
    // Разблокировка хранилища (ввод правильного мастер-пароля)
    bool unlock(const QString &password);
    
    // Блокировка (очистка из памяти)
    void lock();
    
    // Проверка разблокировано ли (мастер-пароль в памяти)
    bool isUnlocked() const;

    // Сохранение пароля для подключения
    void savePassword(const QString &connectionId, const QString &password);
    
    // Получение пароля для подключения
    QString getPassword(const QString &connectionId) const;
    
    // Удаление пароля
    void removePassword(const QString &connectionId);
    
    // Получение всех ID подключений с паролями
    QStringList getConnectionIds() const;

signals:
    void vaultLocked();
    void vaultUnlocked();

private:
    QString encryptPassword(const QString &password) const;
    QString decryptPassword(const QString &encryptedPassword) const;
    QString hashPassword(const QString &password) const;
    void saveVault();
    bool loadVault();

    QString m_masterPasswordHash;
    QString m_masterPassword; // Хранится в памяти только во время работы приложения
    bool m_isUnlocked;
    QMap<QString, QString> m_passwords;
    QString m_vaultPath;
    QString m_hashPath;
};

#endif // PASSWORDMANAGER_H