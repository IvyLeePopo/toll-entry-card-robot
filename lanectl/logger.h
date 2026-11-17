#ifndef LOGGER_H
#define LOGGER_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <QString>
#include <stdio.h>

// ============================================================================
//  日志 (W1 重构版)
//
//  v0 的问题: 单例无锁、每条 fflush 都在调用线程上做、没有分级。
//  现在:
//    * 调用线程只负责把一行塞进队列, 真正落盘由后台线程做
//    * 有 Debug/Info/Warn/Error 四级
//    * 队列有上限, 满了丢最老的并计数 (宁可丢日志, 不能拖垮发卡)
// ============================================================================
class Logger
{
public:
    enum Level
    {
        Debug = 0,
        Info,
        Warn,
        Error
    };

    static Logger *inst();

    void init(const QString &dir = QString());
    void write(Level lv, const QString &msg);
    void flush();
    void shutdown();

    unsigned long droppedCount() const { return m_dropped.load(); }

private:
    Logger();
    ~Logger();

    void workerLoop();
    static const char *levelTag(Level lv);

    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<std::string> m_queue;
    std::thread m_thread;
    std::atomic<bool> m_stop;
    std::atomic<bool> m_started;
    std::atomic<unsigned long> m_dropped;

    std::mutex m_fileMu;
    FILE *m_fp;
    bool m_flushNow;

    static const std::size_t kMaxQueue = 4096;
};

#define LOG(x)  Logger::inst()->write(Logger::Info,  x)
#define LOGD(x) Logger::inst()->write(Logger::Debug, x)
#define LOGW(x) Logger::inst()->write(Logger::Warn,  x)
#define LOGE(x) Logger::inst()->write(Logger::Error, x)

#endif // LOGGER_H
