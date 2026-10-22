#ifndef LEGACYSSH_H
#define LEGACYSSH_H

#include <QString>
#include <QStringList>

QString legacySshOpenSslConfigPath(QString *errorMessage = nullptr);
QStringList legacySshOptions();

#endif
