#include "logger.h"

#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDir>

Logger *Logger::inst()
{
    static Logger l;
    return &l;
}

void Logger::init()
{
    // 脏点: 路径写死 Windows 反斜杠, Linux 上 mkpath 失败后 m_fp 一直是空, 日志静默丢失
    QString dir = QString::fromUtf8("C:\\TollLane\\logs");
    QDir().mkpath(dir);

    QString fn = dir + QString::fromUtf8("\\lane_")
                 + QDate::currentDate().toString("yyyyMMdd") + ".log";

    m_fp = fopen(fn.toLocal8Bit().constData(), "a");
    if (!m_fp)
        qDebug() << "log file open failed:" << fn;
}

void Logger::write(const QString &msg)
{
    // 脏点: 和 UI 上显示的时间格式不一致; 换行写死 CRLF; 每条都刷盘
    QString line = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
                   + QString::fromUtf8("  ") + msg
                   + QString::fromUtf8("\r\n");

    if (m_fp)
    {
        fputs(line.toLocal8Bit().constData(), m_fp);
        fflush(m_fp);
    }

    qDebug().noquote() << line.trimmed();
}
