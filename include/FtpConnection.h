#ifndef FTP_SERVER_POLL_FTPCONNECTION_H
#define FTP_SERVER_POLL_FTPCONNECTION_H

#include <PollMessageEngine.h>
#include <string>
#include <filesystem>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <boost/intrusive/list.hpp>
#include <fcntl.h>

namespace ftp {

using namespace std::string_literals;

//Единственный валидный в данной реализации - ASCII, остальные перечислены, чтобы можно было распознавать команды
enum class RepresentationType : char
{
    A = 'A',    // ASCII
    E = 'E',    // EBCDIC
    I = 'I',    // IMAGE
    L = 'L'     // LOCAL
};

//Единственный валидный в данной реализации - Non-print, остальные перечислены, чтобы можно было распознавать команды
enum class Format : char
{
    N = 'N',    // Non-print
    T = 'T',    // Telnet
    C = 'C',    // Carriage Control
};

//Единственный валидный в данной реализации - STREAM, остальные перечислены, чтобы можно было распознавать команды
enum class Mode : char
{
    S = 'S',    // STREAM
    B = 'B',    // BLOCK
    C = 'C'     // COMPRESSED
};

//Единственный валидный в данной реализации - FILE, остальные перечислены, чтобы можно было распознавать команды
enum class Structure : char
{
    F = 'F',    // FILE
    R = 'R',    // RECORD
    P = 'P'     // PAGE
};

namespace details::helpers {

void setNonBlocking(int fd);

}

class Connection : public boost::intrusive::list_base_hook<>
{
public:
    // FTP-соединение инициализируется на сокете с дескриптором fd,
    // для которого уже выполнен вызов accept().
    // Таким образом, connection гарантированно общается с одним клиентом и
    // не знает о сокете, на котором принимаются соединения.
    // Connection владеет своим fd и закрывает его сам при завершении работы
    explicit Connection(
            int fd
            , std::shared_ptr<messaging::PollMessageEngine>& messageEngine
            , std::filesystem::path root
            , const std::function<void(Connection&)>& connectionCloseCallback)
    : boost::intrusive::list_base_hook<>(), m_fd(fd), m_messageEngine(messageEngine), m_root(std::move(root)), m_notifyOnCloseCallback(std::move(connectionCloseCallback))
    {
        details::helpers::setNonBlocking(m_fd);
        socklen_t addrLen = sizeof(m_socketAddress);
        getsockname(m_fd, reinterpret_cast<sockaddr*>(&m_socketAddress), &addrLen);
        m_dataConnectionAddress = m_socketAddress;
        m_dataConnectionAddress.sin_port = 0;

        auto aliveCriteria = m_isAlive;

        m_defaultBehavior =
                std::make_shared<messaging::CallbackType>(
                        [this, aliveCriteria](int res) mutable
                {
                    if(aliveCriteria->load())
                    {
                        if (res > 0)
                            m_messageEngine->async_read_until(
                                    m_fd, m_msg, std::make_shared<messaging::CallbackType>(
                                            [this, aliveCriteria](int res)
                                            {
                                                if(aliveCriteria->load())
                                                {
                                                    if (res > 0)
                                                        processNewCommand();
                                                    else
                                                        killSelf();
                                                }
                                            }), "\r\n"s);
                        else
                            killSelf();
                    }
                });

        m_repeatedDataSender =
                std::make_shared<messaging::CallbackType>(
                        [this, aliveCriteria](int res) mutable
                {
                    if(aliveCriteria->load())
                    {// Задача этой функции заключается в том,
                        // чтобы поблочно вычитать файл и затем закрыть соединение
                        if (res >= 0)
                        {
                            //Данные успешно отправлены или функция только что вызвана - продолжаем читать и пересылать
                            m_dataBuffer.resize(500, 0);
                            m_dataBuffer.reserve(1000);

                            res = fread(m_dataBuffer.data(), sizeof(char), 500, m_file);
                            if (res > 0)
                            {
                                // Чтение продолжается, отправляем вычитанный блок получателю, заменив \n на \r\n
                                m_dataBuffer.resize(res);
                                replaceNormalEolsToTelnet();
                                // Символы заменили, отправляем данные
                                m_messageEngine->async_write(
                                        m_dataTransmissionFd, m_dataBuffer, m_repeatedDataSender);
                            }
                            else if (res == 0)
                            {
                                // Чтение закончилось
                                m_dataBuffer.clear();
                                closeDataTransmissionSockets();
                                reply("250 Transfer complete");
                            }
                            else
                            {
                                //Ошибка передачи данных - завершаем передачу
                                m_dataBuffer.clear();
                                closeDataTransmissionSockets();
                                reply("450 File action not taken");
                            }
                        }
                        else
                        {
                            // Сокет закрыт клиентом - прерываем передачу
                            m_dataBuffer.clear();
                            closeDataTransmissionSockets();
                            reply("426 Transfer aborted due to connection close");
                        }
                    }
                });

        m_repeatedDataReceiver =
                std::make_shared<messaging::CallbackType>(
                        [this, aliveCriteria](int res) mutable
                {
                    if(aliveCriteria->load())
                    {// Задача этой функции заключается в том,
                        // чтобы поблочно вычитать файл и затем закрыть соединение
                        if (res >= 0)
                        {
                            //Данные успешно отправлены или функция только что вызвана - продолжаем читать и пересылать
                            m_dataBuffer.resize(500, 0);
                            m_dataBuffer.reserve(1000);

                            m_messageEngine->async_read_some(
                                    m_dataTransmissionFd, m_dataBuffer, std::make_shared<messaging::CallbackType>(
                                            [this, aliveCriteria](int res)
                                            {
                                                if(aliveCriteria->load())
                                                {
                                                    if (res > 0)
                                                    {
                                                        // Чтение продолжается, отправляем вычитанный блок на диск, заменив \r\n на \n
                                                        m_dataBuffer.resize(res);
                                                        replaceTelnetEolsToNormal();
                                                        (*m_repeatedDataReceiver)(
                                                                fwrite(
                                                                        m_dataBuffer.data(), sizeof(char), m_dataBuffer.size()
                                                                        , m_file));
                                                    }
                                                    else if (res == 0)
                                                    {
                                                        // Заменяем концы строк
                                                        m_dataBuffer.resize(res);
                                                        replaceTelnetEolsToNormal();
                                                        // Сокет закрыт, дописываем данные и завершаем соединение
                                                        res = fwrite(
                                                                m_dataBuffer.data(), sizeof(char), m_dataBuffer.size(), m_file);
                                                        if (res >= 0)
                                                        {
                                                            // Завершаем передачу, последний кусок данных успешно записан
                                                            closeDataTransmissionSockets();
                                                            reply("250 Transfer complete");
                                                        }
                                                        else
                                                        {
                                                            // Файлу плохо
                                                            closeDataTransmissionSockets();
                                                            reply("450 File action not taken");
                                                        }
                                                    }
                                                    else
                                                    {
                                                        // Произошла ошибка на сокете
                                                        closeDataTransmissionSockets();
                                                        reply("426 Transfer aborted due to connection close");
                                                    }
                                                }
                                            }));
                        }
                        else
                        {
                            // Файлу плохо
                            closeDataTransmissionSockets();
                            reply("450 File action not taken");
                        }
                    }
                });
    }

    void start()
    {
        reply("220 Hello!");
    }

    bool operator==(const Connection& other) const
    {
        return other.m_fd == m_fd;
    }

    ~Connection()
    {
        m_isAlive->store(false);
        close(m_fd);
        if(m_dataTransmissionFd != -1)
            close(m_dataTransmissionFd);
        if(m_dataFd != -1)
            close(m_dataFd);
        if(m_file)
            fclose(m_file);
    }

private:
    void user(const std::string& username);
    void quit();
    void type(RepresentationType representationType, Format format = Format::N);
    void mode(Mode mode);
    void stru(Structure structure);
    void retr(const std::filesystem::path& path);
    void stor(const std::filesystem::path& path);
    void noop();
    void pasv();
    void pwd()
    {
        reply("257 /");
    }
    void list(const std::filesystem::path& path);
    void processNewCommand();
    void killSelf()
    {
        m_notifyOnCloseCallback(*this);
    }
    void reply(const std::string& reply);
    void echo(const std::string& msg);
    void sendFile(int fileFd);
    void recvFile(int fileFd);
    void closeDataTransmissionSockets()
    {
        close(m_dataTransmissionFd);
        m_dataTransmissionFd = -1;
        fclose(m_file);
        m_file = nullptr;
    }
    void replaceNormalEolsToTelnet()
    {
        auto pos = m_dataBuffer.find('\n');
        while (pos != std::string::npos)
        {
            m_dataBuffer.replace(pos, 1, "\r\n");
            pos = m_dataBuffer.find('\n', pos + 2);
        }
    }
    void replaceTelnetEolsToNormal()
    {
        auto pos = m_dataBuffer.find("\r\n");
        while (pos != std::string::npos)
        {
            m_dataBuffer.replace(pos, 2, "\n");
            pos = m_dataBuffer.find("\r\n", pos + 1);
        }
    }


private:
    int m_fd, // Дескриптор, на котором работают управляющее и транспортное соединения.
        m_dataFd = -1, // Дескриптор, на котором будут приниматься соединения для передачи данных
        m_dataTransmissionFd = -1; // Дескриптор, на котором передача данных непосредственно осуществляется
    FILE* m_file = nullptr;
    std::string m_msg, m_dataBuffer;
    bool m_isAuthenticated = false; // Прошел ли пользователь начальную аутентификацию
    std::shared_ptr<messaging::PollMessageEngine> m_messageEngine; // Механизм для обмена сообщениями, должен быть общим для всех сущностей
    std::filesystem::path m_root;
    sockaddr_in m_socketAddress, m_dataConnectionAddress;

    std::shared_ptr<std::atomic_bool> m_isAlive = std::make_shared<std::atomic_bool>(true);

    std::shared_ptr<messaging::CallbackType> m_defaultBehavior;

    std::shared_ptr<messaging::CallbackType> m_repeatedDataSender;

    std::shared_ptr<messaging::CallbackType> m_repeatedDataReceiver;

    std::function<void(Connection&)> m_notifyOnCloseCallback;
};

} //namespace ftp

#endif //FTP_SERVER_POLL_FTPCONNECTION_H
