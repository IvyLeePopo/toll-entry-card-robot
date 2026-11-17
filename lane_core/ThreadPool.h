#ifndef LANE_CORE_THREADPOOL_H
#define LANE_CORE_THREADPOOL_H

// ============================================================================
//  线程池 —— 刻意不依赖 Qt，后面 W4 会把它并进 lane_core 静态库
//
//  用法:
//      ThreadPool pool(4);
//      std::future<int> f = pool.submit([]{ return 42; });
//      int v = f.get();
//
//  设计要点:
//    * std::thread + std::mutex + std::condition_variable + std::atomic
//    * submit 用 packaged_task 包任务, 调用方拿 future 取结果/异常
//    * shutdown 后队列里的任务会被丢弃 (不保证执行), 析构时自动 join
// ============================================================================

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

class ThreadPool
{
public:
    explicit ThreadPool(std::size_t threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    template <typename F>
    std::future<typename std::decay<typename std::result_of<F()>::type>::type>
    submit(F &&f)
    {
        using R = typename std::result_of<F()>::type;

        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
        std::future<R> fut = task->get_future();

        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_stop)
                throw std::runtime_error("ThreadPool::submit on stopped pool");
            m_tasks.emplace([task]() { (*task)(); });
        }
        m_cv.notify_one();
        return fut;
    }

    std::size_t threadCount() const { return m_workers.size(); }
    std::size_t queuedCount() const;
    void shutdown();

private:
    void workerLoop();

    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    mutable std::mutex m_mu;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop;
};

#endif // LANE_CORE_THREADPOOL_H
