#ifndef FTP_SERVER_POLL_POLLMESSAGEENGINE_H
#define FTP_SERVER_POLL_POLLMESSAGEENGINE_H

#include <sys/poll.h>
#include <vector>
#include <string>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <queue>
#include <unistd.h>
#include <mutex>
#include <atomic>

namespace messaging {

using CallbackType = std::function<void(int)>;
using ExtCallbackType = std::function<void(void)>;
template<typename BufferType>
using PredicateType = std::function<std::ptrdiff_t(const BufferType&)>;

class PollMessageEngine {
public:
    PollMessageEngine() = default;

    template<typename BufferType>
    void async_read_some(int fd, BufferType& buffer, const std::shared_ptr<CallbackType>& callback){
        async_read_some_impl(fd, {reinterpret_cast<std::byte *>(buffer.data()), buffer.size()}, callback);
    }

    template<typename BufferType>
    void async_write_some(int fd, BufferType buffer, const std::shared_ptr<CallbackType>& callback){
        async_write_some_impl(fd, {reinterpret_cast<std::byte *>(buffer.data()), buffer.size()}, callback);
    }

    template<typename BufferType>
    void async_write(int fd, BufferType buffer, const std::shared_ptr<CallbackType>& callback){
        async_write_impl(fd, {reinterpret_cast<std::byte *>(buffer.data()), buffer.size()}, callback, 0);
    }

    template<typename BufferType>
    void async_read(int fd, BufferType& buffer, const std::shared_ptr<CallbackType>& callback){
        async_read_impl(fd, {reinterpret_cast<std::byte *>(buffer.data()), buffer.size()}, callback, 0);
    }

    template<typename BufferType>
    void async_read_until(int fd, BufferType& buffer, std::shared_ptr<CallbackType> callback, std::shared_ptr<PredicateType<BufferType>> pred){
        async_read_until_impl(fd, buffer, callback, pred);
    }

    template<typename BufferType, typename PredicateBufferType>
    void async_read_until(int fd, BufferType& buffer, std::shared_ptr<CallbackType> callback, const PredicateBufferType& pred){
        async_read_until_impl(fd, buffer, callback, std::make_shared<PredicateType<PredicateBufferType>>(
                [pred](const BufferType& buf)
                {
                    auto searchRes = std::default_searcher(pred.begin(), pred.end())(buf.begin(), buf.end());
                    return searchRes.second - searchRes.first;
                }));
    }

    void async_accept(int fd, std::shared_ptr<CallbackType> callback);

    ExtCallbackType waitForEvent();

    void interrupt()
    {
        m_interruptanceFlag.store(true);
    }

    void release()
    {
        m_interruptanceFlag.store(false);
    }

    ~PollMessageEngine()
    {
        m_isAlive->store(false);
    }

private:

    void async_read_some_impl(int fd, std::span<std::byte> buffer, std::shared_ptr<CallbackType> callback);

    void async_write_some_impl(int fd, std::span<std::byte> buffer, std::shared_ptr<CallbackType> callback);

    void async_read_impl(int fd, std::span<std::byte> buffer, std::shared_ptr<CallbackType> callback, int offset = 0);

    void async_write_impl(int fd, std::span<std::byte> buffer, std::shared_ptr<CallbackType> callback, int offset = 0);

    template<typename BufferType>
    void async_read_until_impl(int fd, BufferType& buffer, std::shared_ptr<CallbackType> callback, std::shared_ptr<PredicateType<BufferType>> pred)
    {
        //Для начала, проверим, не совпало ли уже
        if (auto matchLen = (*pred)(buffer); matchLen > 0) {
            (*callback)(matchLen);
            return;
        }
        if (buffer.size() == buffer.max_size()) {
            (*callback)(-ENOMEM);
            return;
        }
        if (buffer.size() == buffer.capacity()) {
            buffer.push_back(*buffer.data());
            buffer.pop_back();
        }

        std::size_t len = std::min(buffer.max_size(), buffer.capacity()) - buffer.size();
        buffer.resize(buffer.size() + len);
        auto aliveCriteria = m_isAlive;
        async_read_some_impl(
                fd,
                {reinterpret_cast<std::byte *>(buffer.data()) + buffer.size() - len, len},
                std::make_shared<CallbackType>(
                        [=, &buffer](int res) mutable
                        {
                            if(aliveCriteria->load())
                            {
                                buffer.resize(buffer.size() - len + res);
                                if (res <= 0)
                                {
                                    (*callback)(res);
                                }
                                else
                                {
                                    async_read_until(fd, buffer, callback, pred);
                                }
                            }
                        }));
    }

    struct innerType {
        pollfd m_fd;
        std::shared_ptr<CallbackType> m_callback;
        std::span<std::byte> m_buffer;
        int m_associatedIndex;
    };

    std::vector<innerType> m_queries;
    std::vector<pollfd> m_fds;
    std::queue<ExtCallbackType> m_readyForOperationQueue;
    std::mutex m_queryMutex;
    ExtCallbackType m_emptyCallback = [](){};
    std::atomic_bool m_interruptanceFlag = false;

    std::shared_ptr<std::atomic_bool> m_isAlive = std::make_shared<std::atomic_bool>(true);
};

} //namespace messaging

#endif //FTP_SERVER_POLL_POLLMESSAGEENGINE_H
