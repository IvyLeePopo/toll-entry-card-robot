#ifndef LANE_CORE_COMMANDTRACKER_H
#define LANE_CORE_COMMANDTRACKER_H

// ============================================================================
//  命令-结果配对器
//
//  解决 v0 里那个问题: 发一条命令后只能靠一个 QTimer 干等 8 秒,
//  超时了也不知道是哪条命令超的。
//
//  现在: 发命令时登记一个 waiter, 网络线程收到 B1 就 complete() 兑现,
//        工作线程拿着 future 带超时地等 —— 每条命令有自己的 deadline。
//
//  纯 C++ (无 Qt), 可直接单测。
// ============================================================================

#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>

struct CmdResult
{
    int cmd = 0;
    int rc = -1;
    bool ok = false;
    bool timeout = false;
};

class CommandTracker
{
public:
    struct Waiter
    {
        std::uint64_t id = 0;
        int cmd = 0;
        std::future<CmdResult> future;
    };

    // 登记一条待完成命令, 返回 waiter (future 由调用方等)
    Waiter begin(int cmd);

    // 网络层收到 B1 时调用; 返回 false 表示没有对应 waiter (fire-and-forget 命令)
    bool complete(int cmd, int rc);

    // 超时后摘掉 waiter, 防止泄漏
    void drop(std::uint64_t id);

    // 断线时把所有挂着的命令一次性失败掉
    void failAllPending(int rc);

    std::size_t pending() const;

private:
    struct Entry
    {
        int cmd = 0;
        std::shared_ptr<std::promise<CmdResult>> promise;
    };

    mutable std::mutex m_mu;
    std::uint64_t m_nextId = 1;
    std::unordered_map<std::uint64_t, Entry> m_byId;
    std::unordered_multimap<int, std::uint64_t> m_byCmd;
};

#endif // LANE_CORE_COMMANDTRACKER_H
