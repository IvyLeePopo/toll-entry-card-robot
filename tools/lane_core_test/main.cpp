// lane_core 验收程序 (W1):
//   1) 线程池提交 1000 个任务, 全部能取回结果
//   2) 任务抛异常能透过 future 传出来
//   3) 任务确实并行执行 (峰值并发 > 1)
//   4) CommandTracker: 正常兑现 / 超时 / 断线失败 / 同命令多条
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

#include "CommandTracker.h"
#include "ThreadPool.h"

static int g_fail = 0;

static void check(bool cond, const char *what)
{
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        ++g_fail;
}

int main()
{
    std::printf("=========== lane_core test ===========\n");
    std::printf("hardware_concurrency = %u\n", std::thread::hardware_concurrency());

    ThreadPool pool(4);
    std::printf("pool threads = %zu\n", pool.threadCount());

    // ---------------- 1. 1000 个任务 ----------------
    {
        std::printf("\n[1] 1000 tasks\n");
        std::vector<std::future<long long>> futures;
        futures.reserve(1000);

        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; ++i)
            futures.push_back(pool.submit([i]() { return (long long)i * i; }));

        long long sum = 0;
        for (auto &f : futures)
            sum += f.get();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();

        long long expect = 0;
        for (int i = 0; i < 1000; ++i)
            expect += (long long)i * i;

        std::printf("  sum = %lld, expect = %lld, %lld ms\n", sum, expect, (long long)ms);
        check(sum == expect, "all 1000 results correct");
        check(pool.queuedCount() == 0, "queue drained");
    }

    // ---------------- 2. 异常传递 ----------------
    {
        std::printf("\n[2] exception through future\n");
        auto f = pool.submit([]() -> int { throw std::runtime_error("boom"); });
        bool caught = false;
        try
        {
            f.get();
        }
        catch (const std::exception &e)
        {
            caught = true;
            std::printf("  caught: %s\n", e.what());
        }
        check(caught, "exception propagated, worker still alive");

        auto f2 = pool.submit([]() { return 7; });
        check(f2.get() == 7, "pool still works after exception");
    }

    // ---------------- 3. 并行度 ----------------
    {
        std::printf("\n[3] real parallelism\n");
        std::atomic<int> running(0);
        std::atomic<int> peak(0);

        std::vector<std::future<void>> fs;
        for (int i = 0; i < 8; ++i)
        {
            fs.push_back(pool.submit([&running, &peak]() {
                int cur = ++running;
                int prev = peak.load();
                while (cur > prev && !peak.compare_exchange_weak(prev, cur))
                {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                --running;
            }));
        }
        for (auto &f : fs)
            f.get();

        std::printf("  peak concurrency = %d\n", peak.load());
        check(peak.load() > 1, "tasks really run in parallel");
        check(peak.load() <= 4, "never exceeds pool size");
    }

    // ---------------- 4. CommandTracker ----------------
    {
        std::printf("\n[4] CommandTracker\n");
        CommandTracker tracker;

        // 4.1 正常兑现
        auto w = tracker.begin(0x04);
        check(tracker.complete(0x04, 0), "complete finds waiter");
        CmdResult r = w.future.get();
        check(r.cmd == 0x04 && r.ok && !r.timeout, "waiter receives success");
        check(!tracker.complete(0x04, 0), "second complete returns false");
        check(tracker.pending() == 0, "nothing pending");

        // 4.2 超时
        auto w2 = tracker.begin(0x05);
        auto st = w2.future.wait_for(std::chrono::milliseconds(30));
        check(st == std::future_status::timeout, "wait_for times out");
        tracker.drop(w2.id);
        check(tracker.pending() == 0, "drop removes waiter");

        // 4.3 断线一次性失败
        auto w3 = tracker.begin(0x06);
        tracker.failAllPending(9);
        CmdResult r3 = w3.future.get();
        check(r3.rc == 9 && !r3.ok, "failAllPending delivers rc");

        // 4.4 同一条命令有多条在等 (对应"全部备卡"连发 4 条)
        auto a = tracker.begin(0x03);
        auto b = tracker.begin(0x03);
        tracker.complete(0x03, 0);
        bool ra = (a.future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready);
        bool rb = (b.future.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready);
        check(ra != rb, "same cmd: exactly one waiter completed");
        tracker.complete(0x03, 0);
        check(tracker.pending() == 0, "both waiters consumed");
    }

    // ---------------- 5. 线程池 + 超时 (实际用法) ----------------
    {
        std::printf("\n[5] pooled await with timeout\n");
        CommandTracker tracker;
        auto w = tracker.begin(0x10);

        auto f = pool.submit([&tracker, w = std::move(w)]() mutable {
            CmdResult r;
            if (w.future.wait_for(std::chrono::milliseconds(40)) == std::future_status::ready)
            {
                r = w.future.get();
            }
            else
            {
                tracker.drop(w.id);
                r.cmd = w.cmd;
                r.rc = 5;
                r.timeout = true;
            }
            return r;
        });

        CmdResult r = f.get();
        std::printf("  result: cmd=0x%02X rc=%d timeout=%d\n", r.cmd, r.rc, (int)r.timeout);
        check(r.timeout && r.rc == 5, "worker thread reports timeout by itself");
        check(pool.queuedCount() == 0, "no leftovers");
    }

    pool.shutdown();
    check(pool.threadCount() == 0, "pool shutdown joins threads");

    std::printf("\n=========== %s (%d failed) ===========\n", g_fail == 0 ? "ALL PASS" : "FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
