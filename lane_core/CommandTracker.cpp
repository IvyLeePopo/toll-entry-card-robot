#include "CommandTracker.h"

#include <utility>

CommandTracker::Waiter CommandTracker::begin(int cmd)
{
    auto promise = std::make_shared<std::promise<CmdResult>>();

    std::lock_guard<std::mutex> lk(m_mu);
    std::uint64_t id = m_nextId++;

    Entry e;
    e.cmd = cmd;
    e.promise = promise;
    m_byId.emplace(id, e);
    m_byCmd.emplace(cmd, id);

    Waiter w;
    w.id = id;
    w.cmd = cmd;
    w.future = promise->get_future();
    return w;
}

bool CommandTracker::complete(int cmd, int rc)
{
    std::shared_ptr<std::promise<CmdResult>> promise;

    {
        std::lock_guard<std::mutex> lk(m_mu);

        auto it = m_byCmd.find(cmd);
        if (it == m_byCmd.end())
            return false;

        std::uint64_t id = it->second;
        auto e = m_byId.find(id);
        if (e == m_byId.end())
        {
            m_byCmd.erase(it);
            return false;
        }

        promise = e->second.promise;
        m_byId.erase(e);
        m_byCmd.erase(it);
    }

    // 兑现放在锁外, 避免唤醒的线程立刻抢锁
    CmdResult r;
    r.cmd = cmd;
    r.rc = rc;
    r.ok = (rc == 0);
    r.timeout = false;
    promise->set_value(r);
    return true;
}

void CommandTracker::drop(std::uint64_t id)
{
    std::lock_guard<std::mutex> lk(m_mu);

    auto e = m_byId.find(id);
    if (e == m_byId.end())
        return;

    int cmd = e->second.cmd;
    m_byId.erase(e);

    auto range = m_byCmd.equal_range(cmd);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second == id)
        {
            m_byCmd.erase(it);
            break;
        }
    }
}

void CommandTracker::failAllPending(int rc)
{
    std::unordered_map<std::uint64_t, Entry> victims;

    {
        std::lock_guard<std::mutex> lk(m_mu);
        victims.swap(m_byId);
        m_byCmd.clear();
    }

    for (auto &kv : victims)
    {
        CmdResult r;
        r.cmd = kv.second.cmd;
        r.rc = rc;
        r.ok = false;
        r.timeout = false;
        try
        {
            kv.second.promise->set_value(r);
        }
        catch (...)
        {
            // promise 已经被别的路径兑现过, 忽略
        }
    }
}

std::size_t CommandTracker::pending() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_byId.size();
}
