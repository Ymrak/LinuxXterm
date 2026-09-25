#ifndef CONNECTION_H
#define CONNECTION_H

#include <QString>
#include <QJsonObject>

class Connection
{
public:
    Connection();
    Connection(const QString &name, const QString &host, int port = 22,
               const QString &login = "", const QString &password = "",
               const QString &privateKeyPath = "", bool useKey = false);

    // Геттеры
    QString getId() const;
    QString getName() const;
    QString getHost() const;
    int getPort() const;
    QString getLogin() const;
    QString getPassword() const;
    QString getPrivateKeyPath() const;
    bool getUseKey() const;

    // Сеттеры
    void setId(const QString &id);
    void setName(const QString &name);
    void setHost(const QString &host);
    void setPort(int port);
    void setLogin(const QString &login);
    void setPassword(const QString &password);
    void setPrivateKeyPath(const QString &path);
    void setUseKey(bool useKey);

    // Сериализация в JSON (пароль теперь НЕ шифруем!)
    QJsonObject toJson() const;
    static Connection fromJson(const QJsonObject &json);

private:
    QString m_id;
    QString m_name;
    QString m_host;
    int m_port;
    QString m_login;
    QString m_password;  // Теперь хранится в открытом виде (используется только для передачи в PasswordManager)
    QString m_privateKeyPath;
    bool m_useKey;

    static QString generateId();
};

#endif // CONNECTION_H