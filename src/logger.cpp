#include "logger.h"
#include <QDir>
#include <QDebug>
#include <iostream>

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::Logger()
{
    // Создаём директорию для логов
    QString logDir = QDir::homePath() + "/.config/linuxxterm";
    QDir().mkpath(logDir);
    
    // Открываем файл лога
    m_logFile.setFileName(logDir + "/linuxxterm.log");
    if (m_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_logStream.setDevice(&m_logFile);
    }
}

Logger::~Logger()
{
    if (m_logFile.isOpen()) {
        m_logFile.close();
    }
}

void Logger::log(const QString &message)
{
    QMutexLocker locker(&m_mutex);
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    QString logLine = QString("[%1] %2").arg(timestamp, message);
    
    // Выводим в консоль
    qDebug().noquote() << logLine;
    
    // Записываем в файл
    if (m_logStream.device()) {
        m_logStream << logLine << "\n";
        m_logStream.flush();
    }
}

void Logger::error(const QString &message)
{
    log("[ОШИБКА] " + message);
}

void Logger::info(const QString &message)
{
    log("[ИНФО] " + message);
}
