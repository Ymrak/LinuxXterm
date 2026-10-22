#ifndef SSHPASSPROXY_H
#define SSHPASSPROXY_H

#include <QString>
#include <QStringList>

class QLocalSocket;
class QLocalServer;

int runSshpassProxy(int argc, char *argv[]);
QString createSshpassProxySocketName();
QStringList makeSshpassProxyArguments(const QString &socketName, const QString &sshpassPath,
                                      const QStringList &sshpassArguments);
bool isSshpassProxyPeer(QLocalSocket *socket, qint64 expectedProcessId);
QLocalSocket *waitForSshpassProxy(QLocalServer *server, qint64 expectedProcessId,
                                  int timeoutMilliseconds);
bool sendSshpassProxyPassword(QLocalSocket *socket, const QString &password);

#endif
