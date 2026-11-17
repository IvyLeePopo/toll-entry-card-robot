#include "ThreadPool.h"

ThreadPool::ThreadPool(std::size_t threads)
    : m_stop(false)
{
    if (threads == 0)
    {
        threads = std::thread::hardware_concurrency();
        if (threads == 0)
            threads = 2;            // hardware_concurrency 可能返回 0
    }

    m_workers.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i)
        m_workers.emplace_back([this]() { workerLoop(); });
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::workerLoop()
{
    for (;;)
    {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lk(m_mu);
            m_cv.wait(lk, [this]() { return m_stop.load() || !m_tasks.empty(); });

            // 停机时先把队列里剩下的任务执行完再退出, 避免提交了却永远不完成
            if (m_stop.load() && m_tasks.empty())
                return;

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        // 任务里的异常由 packaged_task 收进 future, 不会打穿线程
        task();
    }
}

std::size_t ThreadPool::queuedCount() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_tasks.size();
}

void ThreadPool::shutdown()
{
    bool expected = false;
    if (!m_stop.compare_exchange_strong(expected, true))
        return;                     // 已经停过了

    m_cv.notify_all();

    for (std::size_t i = 0; i < m_workers.size(); ++i)
    {
        if (m_workers[i].joinable())
            m_workers[i].join();
    }
    m_workers.clear();
}
