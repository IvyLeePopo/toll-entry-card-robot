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

Logger::Logger()
    : m_stop(false)
    , m_started(false)
    , m_dropped(0)
    , m_fp(0)
    , m_flushNow(false)
{
}

Logger::~Logger()
{
    shutdown();
}

const char *Logger::levelTag(Level lv)
{
    switch (lv)
    {
    case Debug:
        return "D";
    case Info:
        return "I";
    case Warn:
        return "W";
    case Error:
        return "E";
    }
    return "?";
}

void Logger::init(const QString &dir)
{
    if (m_started.load())
        return;

    QString path = dir;
    if (path.isEmpty())
        path = QString::fromUtf8("C:\\TollLane\\logs");

    QDir().mkpath(path);

    QString fn = path + QString::fromUtf8("\\lane_")
                 + QDate::currentDate().toString("yyyyMMdd") + ".log";

    m_fp = fopen(fn.toLocal8Bit().constData(), "a");
    if (!m_fp)
        qDebug() << "log file open failed:" << fn;

    m_thread = std::thread([this]() { workerLoop(); });
    m_started = true;
}

void Logger::write(Level lv, const QString &msg)
{
    if (!m_started.load())
    {
        // init 之前也要能看见
        qDebug().noquote() << msg;
        return;
    }

    QString line = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
                   + QString::fromUtf8(" [") + QString::fromLatin1(levelTag(lv)) + QString::fromUtf8("] ")
                   + msg;

    // W1: 日志文件统一写 UTF-8, 不再受系统区域设置影响
    //     (原来 toLocal8Bit + 手写 CRLF, 换个机器就乱码)
    std::string raw = line.toUtf8().constData();

    {
        std::lock_guard<std::mutex> lk(m_mu);

        if (m_queue.size() >= kMaxQueue)
        {
            m_queue.pop_front();          // 丢最老的
            m_dropped.fetch_add(1);
        }
        m_queue.push_back(raw);
    }

    m_cv.notify_one();
}

void Logger::flush()
{
    if (!m_started.load())
        return;

    std::unique_lock<std::mutex> lk(m_mu);
    m_flushNow = true;
    m_cv.notify_all();

    // 等后台把队列吃空
    m_cv.wait(lk, [this]() { return m_queue.empty() && !m_flushNow; });
}

void Logger::workerLoop()
{
    for (;;)
    {
        std::string line;

        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this]() { return m_stop.load() || !m_queue.empty() || m_flushNow; });

            if (m_queue.empty() && m_flushNow)
            {
                m_flushNow = false;
                m_cv.notify_all();
            }

            if (m_stop.load() && m_queue.empty())
                return;

            if (!m_queue.empty())
            {
                line = m_queue.front();
                m_queue.pop_front();
            }
            else if (m_stop.load())
            {
                return;
            }
        }

        if (line.empty())
            continue;

        {
            std::lock_guard<std::mutex> flk(m_fileMu);
            if (m_fp)
            {
                fwrite(line.data(), 1, line.size(), m_fp);
                fputc('\n', m_fp);        // 换行由落盘线程补, 不再依赖平台
                fflush(m_fp);
            }
        }

        qDebug().noquote() << QString::fromUtf8(line.c_str());
    }
}

void Logger::shutdown()
{
    if (!m_started.load())
        return;

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_stop = true;
    }
    m_cv.notify_all();

    if (m_thread.joinable())
        m_thread.join();

    {
        std::lock_guard<std::mutex> flk(m_fileMu);
        if (m_fp)
        {
            fclose(m_fp);
            m_fp = 0;
        }
    }

    m_started = false;
}
