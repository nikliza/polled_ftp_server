#ifndef FTP_SERVER_POLL_FTPSERVER_H
#define FTP_SERVER_POLL_FTPSERVER_H

#include <boost/asio/thread_pool.hpp>
#include <boost/intrusive/list.hpp>
#include <FtpConnection.h>

namespace ftp {

class Server
{
public:
    //Сокет, передаваемый в конструктор сервера, должен быть доведен до готовности принимать соединения
    explicit Server(int socketFd, const std::filesystem::path& root, int threadCount = 1)
    : m_socketFd(socketFd)
    , m_threadCount(threadCount)
    , m_root(root)
    , m_threadPool(threadCount)
    , m_messageEngine(std::make_shared<messaging::PollMessageEngine>()) {}

    void start()
    {
        auto aliveCriteria = m_isAlive;
        m_isUp.store(true);
        m_messageEngine->release();
        // Сервер рекурсивно получает и обрабатывает новые соединения
        handleNewConnections();
        // И забрасывает в пул новые коллбеки для обработки
        m_threadPool.executor().post(
                [this, aliveCriteria]()
                {
                    while(aliveCriteria->load())
                    {
                        m_threadPool.executor().post(m_messageEngine->waitForEvent(), std::allocator<void>());
                    }
                }, std::allocator<void>());
    }

    void stop()
    {
        m_isAlive->store(false);
        m_threadPool.stop();
        m_messageEngine->interrupt();
        m_connectionList.clear_and_dispose(std::default_delete<Connection>());
        m_threadPool.join();
    }

    ~Server()
    {
        m_isAlive->store(false);
        m_threadPool.stop();
        m_messageEngine->interrupt();
        m_connectionList.clear_and_dispose(std::default_delete<Connection>());
        close(m_socketFd);
        m_threadPool.join();
    }
private:

    void handleNewConnections()
    {
        auto aliveCriteria = m_isAlive;
        m_messageEngine->async_accept(
                m_socketFd,
                std::make_shared<messaging::CallbackType>(
                        [this, aliveCriteria](int res)
                        {
                            if(aliveCriteria->load())
                            {
                                if (res >= 0)
                                {
                                    // Accept() прошел успешно, создаем и запускаем новое соединение
                                    auto *connection = new Connection(
                                            res,
                                            m_messageEngine,
                                            m_root,
                                            [this, aliveCriteria](Connection &connection)
                                            {
                                                if(aliveCriteria->load())
                                                {
                                                    m_connectionList.erase_and_dispose(
                                                            m_connectionList.iterator_to(connection)
                                                            , std::default_delete<Connection>());
                                                }
                                            });
                                    m_connectionList.push_back(*connection);
                                    connection->start();
                                    handleNewConnections();
                                }
                                else
                                    stop();
                            }
                        }));
    }

private:
    boost::asio::thread_pool m_threadPool;
    boost::intrusive::list<Connection> m_connectionList;
    std::filesystem::path m_root;
    std::shared_ptr<messaging::PollMessageEngine> m_messageEngine;
    int m_socketFd, m_threadCount;

    std::atomic_bool m_isUp;
    std::shared_ptr<std::atomic_bool> m_isAlive = std::make_shared<std::atomic_bool>(true);
};

} //namespace ftp

#endif //FTP_SERVER_POLL_FTPSERVER_H
