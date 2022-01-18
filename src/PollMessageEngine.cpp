#include <PollMessageEngine.h>
#include <arpa/inet.h>

namespace messaging {

void PollMessageEngine::async_read_some_impl(
        int fd, std::span<std::byte> buffer, std::shared_ptr<CallbackType> callback)
{
    int op_res = read(fd, buffer.data(), buffer.size());
    if (op_res >= 0 || errno != EWOULDBLOCK)
    {
        // Операция завершилась успешно сразу,
        // либо возникла ошибка, не связанная с
        // блокировкой управления;
        // смысла ждать ее доступности через poll нет
        (*callback)(op_res);
        return;
    }
    {
        auto queriesLock = std::lock_guard(m_queryMutex);
        auto aliveCriteria = m_isAlive;
        // Выполнение операции приведет к
        // блокировке потока исполнения,
        // нужно ждать доступности через poll
        m_queries.push_back(
                {
                        {fd, POLLIN, 0},
                        std::make_shared<CallbackType>(
                                [=](int res) mutable
                                {
                                    if(aliveCriteria->load())
                                        async_read_some_impl(fd, buffer, callback);
                                }),
                        buffer,
                        -1
                });
    }
}

void PollMessageEngine::async_write_some_impl(
        int fd, std::span<std::byte> buffer, std::shared_ptr<CallbackType> callback)
{
    int op_res = write(fd, buffer.data(), buffer.size());
    if (op_res >= 0 || errno != EWOULDBLOCK)
    {
        // Операция завершилась успешно сразу,
        // либо возникла ошибка, не связанная с
        // блокировкой управления;
        // смысла ждать ее доступности через poll нет
        (*callback)(op_res);
        return;
    }
    {
        auto queriesLock = std::lock_guard(m_queryMutex);
        auto aliveCriteria = m_isAlive;
        // Выполнение операции приведет к
        // блокировке потока исполнения,
        // нужно ждать доступности через poll
        m_queries.push_back(
                {
                        {fd, POLLOUT, 0},
                        std::make_shared<CallbackType>(
                                [=](int res) mutable
                                {
                                    if(aliveCriteria->load())
                                        async_write_some_impl(fd, buffer, callback);
                                }),
                        buffer,
                        -1
                });
    }
}

void PollMessageEngine::async_read_impl(
        int fd, std::span<std::byte> buffer, std::shared_ptr<CallbackType> callback, int offset)
{
    auto aliveCriteria = m_isAlive;
    async_read_some_impl(
            fd, buffer, std::make_shared<CallbackType>(
                    [=](int res) mutable
                    {
                        if(aliveCriteria->load())
                        {
                            if (res == buffer.size() || res <= 0)
                            {
                                (*callback)(res >= 0 ? offset + res : res);
                            }
                            else
                            {
                                async_read_impl(
                                        fd, buffer.subspan(res), std::move(callback), offset + res
                                );
                            }
                        }
                    }));
}

void PollMessageEngine::async_write_impl(
        int fd, std::span<std::byte> buffer, std::shared_ptr<CallbackType> callback, int offset)
{
    auto aliveCriteria = m_isAlive;
    async_write_some_impl(
            fd, buffer, std::make_shared<CallbackType>(
                    [=](int res) mutable
                    {
                        if(aliveCriteria->load())
                        {
                            if (res == buffer.size() || res <= 0)
                            {
                                (*callback)(res >= 0 ? offset + res : res);
                            }
                            else
                            {
                                async_write_impl(
                                        fd, buffer.subspan(res), std::move(callback), offset + res
                                );
                            }
                        }
                    }));
}

void PollMessageEngine::async_accept(int fd, std::shared_ptr<CallbackType> callback)
{
    sockaddr_in addr;
    socklen_t addrLen = sizeof addr;
    int op_res = accept(fd, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (op_res > 0 || errno != EWOULDBLOCK)
    {
        // Операция завершилась успешно сразу,
        // либо возникла ошибка, не связанная с
        // блокировкой управления;
        // смысла ждать ее доступности через poll нет
        (*callback)(op_res);
        return;
    }
    {
        auto queriesLock = std::lock_guard(m_queryMutex);
        auto aliveCriteria = m_isAlive;
        // Выполнение операции приведет к
        // блокировке потока исполнения,
        // нужно ждать доступности через poll
        m_queries.push_back(
                {
                        {fd, POLLIN, 0},
                        std::make_shared<CallbackType>(
                                [=](int res) mutable
                                {
                                    if(aliveCriteria->load())
                                    {
                                        if (res >= 0)
                                            async_accept(fd, std::move(callback));
                                        else
                                            (*callback)(res);
                                    }
                                }),
                        {},
                        -1
                });
    }
}

ExtCallbackType PollMessageEngine::waitForEvent()
{
    if (m_readyForOperationQueue.empty())
    {
        do
        {
            m_queryMutex.lock();
            m_fds.clear();
            for (auto &entry: m_queries)
            {
                m_fds.push_back(entry.m_fd);
                entry.m_associatedIndex = m_fds.size() - 1;
            }
            m_queryMutex.unlock();
        } while(poll(m_fds.data(), m_fds.size(), 1) == 0 && !m_interruptanceFlag.load());
        {
            auto queriesLock = std::lock_guard(m_queryMutex);
            while (!m_fds.empty())
            {
                auto el = m_fds.back();
                m_fds.pop_back();
                if (el.revents != 0)
                {
                    auto entryIterator = std::find_if(
                            m_queries.cbegin(), m_queries.cend(), [i = m_fds.size()](auto obj)
                            {
                                return obj.m_associatedIndex == i;
                            });
                    auto entryCallback = entryIterator->m_callback;
                    m_readyForOperationQueue.push(
                            [entryCallback]() mutable
                            { (*entryCallback)(0); });
                    m_queries.erase(entryIterator);
                }
            }
        }
    }
    auto cb = m_readyForOperationQueue.empty() ? [](){} : m_readyForOperationQueue.front();
    if(!m_readyForOperationQueue.empty())
        m_readyForOperationQueue.pop();
    return cb;
}

}