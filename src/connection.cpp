#include "connection.h"
#include <QUuid>

Connection::Connection()
    : m_port(22)
    , m_useKey(false)
{
    m_id = generateId();
}

Connection::Connection(const QString &name, const QString &host, int port,
                       const QString &login, const QString &password,
                       const QString &privateKeyPath, bool useKey)
    : m_name(name)
    , m_host(host)
    , m_port(port)
    , m_login(login)
    , m_password(password)
    , m_privateKeyPath(privateKeyPath)
    , m_useKey(useKey)
{
    m_id = generateId();
}

// Геттеры
QString Connection::getId() const { return m_id; }
QString Connection::getName() const { return m_name; }
QString Connection::getHost() const { return m_host; }
int Connection::getPort() const { return m_port; }
QString Connection::getLogin() const { return m_login; }
QString Connection::getPassword() const { return m_password; }
QString Connection::getPrivateKeyPath() const { return m_privateKeyPath; }
bool Connection::getUseKey() const { return m_useKey; }

// Сеттеры
void Connection::setId(const QString &id) { m_id = id; }
void Connection::setName(const QString &name) { m_name = name; }
void Connection::setHost(const QString &host) { m_host = host; }
void Connection::setPort(int port) { m_port = port; }
void Connection::setLogin(const QString &login) { m_login = login; }
void Connection::setPassword(const QString &password) { m_password = password; }
void Connection::setPrivateKeyPath(const QString &path) { m_privateKeyPath = path; }
void Connection::setUseKey(bool useKey) { m_useKey = useKey; }

// Сериализация в JSON (пароль хранится в открытом виде, т.к. основная защита — PasswordManager)
QJsonObject Connection::toJson() const
{
    QJsonObject json;
    json["id"] = m_id;
    json["name"] = m_name;
    json["host"] = m_host;
    json["port"] = m_port;
    json["login"] = m_login;
    json["password"] = m_password;  // Теперь без шифрования
    json["privateKeyPath"] = m_privateKeyPath;
    json["useKey"] = m_useKey;
    return json;
}

Connection Connection::fromJson(const QJsonObject &json)
{
    Connection conn;
    conn.setId(json["id"].toString());
    conn.setName(json["name"].toString());
    conn.setHost(json["host"].toString());
    conn.setPort(json["port"].toInt(22));
    conn.setLogin(json["login"].toString());
    conn.setPassword(json["password"].toString());  // Без расшифровки
    conn.setPrivateKeyPath(json["privateKeyPath"].toString());
    conn.setUseKey(json["useKey"].toBool(false));
    return conn;
}

QString Connection::generateId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}