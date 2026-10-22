#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>

class Logger
{
public:
    static Logger& instance();
    
    void log(const QString &message);
    void error(const QString &message);
    void info(const QString &message);

private:
    Logger();
    ~Logger();
    
    QFile m_logFile;
    QTextStream m_logStream;
    QMutex m_mutex;
};

#define LOG_MSG(msg) Logger::instance().log(msg)
#define LOG_ERR(msg) Logger::instance().error(msg)
#define LOG_INFO(msg) Logger::instance().info(msg)

#endif
