#include <FtpConnection.h>

namespace ftp {


namespace details {

namespace helpers {

// Функция выбрасывает исключения при ошибках конструирования desiredPath и,
// если desiredPath после нормализации содержит переходы вверх по дереву директорий,
// выходящие за пределы каталога.
// В случае, если всё хорошо, возвращает абсолютный путь относительно root.
std::filesystem::path validatePath(const std::string &pathString)
{
    std::filesystem::path desiredPath;
    desiredPath = std::filesystem::path(pathString).lexically_normal();
    std::string tmpPathString = desiredPath.string();
    if (std::count(desiredPath.begin(), desiredPath.end(), std::filesystem::path("..")) != 0
        || std::count(tmpPathString.begin(), tmpPathString.end(), '/') != 0)
        throw std::exception();
    return desiredPath;
}

void setNonBlocking(int fd)
{
    int flags;
    do
    {
        flags = fcntl(fd, F_GETFL, 0);
    } while(flags == -1);
    flags = flags | O_NONBLOCK;
    do {} while(fcntl(fd, F_SETFL, flags) < 0);
}

} //namespace helpers

// Пример для char_traits на cppreference
struct ci_char_traits : public std::char_traits<char> {
    static char to_upper(char ch) {
        return std::toupper((unsigned char) ch);
    }
    static bool eq(char c1, char c2) {
        return to_upper(c1) == to_upper(c2);
    }
    static bool lt(char c1, char c2) {
        return to_upper(c1) <  to_upper(c2);
    }
    static int compare(const char* s1, const char* s2, std::size_t n) {
        while ( n-- != 0 ) {
            if ( to_upper(*s1) < to_upper(*s2) ) return -1;
            if ( to_upper(*s1) > to_upper(*s2) ) return 1;
            ++s1; ++s2;
        }
        return 0;
    }
    static const char* find(const char* s, std::size_t n, char a) {
        auto const ua (to_upper(a));
        while ( n-- != 0 )
        {
            if (to_upper(*s) == ua)
                return s;
            s++;
        }
        return nullptr;
    }
};

using ci_string = std::basic_string<char, ci_char_traits>;

}// namespace details

void Connection::user(const std::string &username)
{
    details::ci_string ci_username(username.c_str());
    if (ci_username == "anonymous")
    {
        m_isAuthenticated = true;
        reply("230 Log in successful");
    }
    else
    {
        //Попытка повторной аутентификации после успешной также сбрасывает текущий статус аутентификации
        m_isAuthenticated = false;
        reply("501 Incorrect user name");
    }
}

void Connection::quit()
{
    echo("221 Bye!");
    killSelf();
}

void Connection::type(RepresentationType representationType, Format format)
{
    if(representationType != RepresentationType::A || format != Format::N)
        reply("504 Command not implemented for specified value");
    else
        reply("200 Type changed");
}

void Connection::mode(Mode mode)
{
    if(mode != Mode::S)
        reply("504 Command not implemented for specified value");
    else
        reply("200 Type changed");
}

void Connection::stru(Structure structure)
{
    if(structure != Structure::F)
        reply("504 Command not implemented for specified value");
    else
        reply("200 Type changed");
}

void Connection::retr(const std::filesystem::path &path)
{
    if(
            !exists(path)
            || is_directory(path))
    {
        reply("534 Request denied");
        return;
    }
    m_file = fopen((path).string().c_str(), "r");
    details::helpers::setNonBlocking(fileno(m_file));
    sendFile(fileno(m_file));
}

void Connection::stor(const std::filesystem::path &path)
{
    if(path.parent_path()/"" != m_root || !exists(path))
    {
        reply("534 Request denied");
        return;
    }
    m_file = fopen((path).string().c_str(), "w");
    recvFile(fileno(m_file));
}

void Connection::noop()
{
    reply("200 Ok");
}

void Connection::pasv()
{
    if(m_dataFd < 0)
    {
        // Если PASV уже вызывалось в рамках данного соединения,
        // то существует неотрицательный сокет dataFd и смысла его пересоздавать нет
        int systemOperationRes = 0;

        socklen_t addrLen = sizeof(m_dataConnectionAddress);
        do
        {
            m_dataFd = socket(AF_INET, SOCK_STREAM, 0);
        } while (m_dataFd < 0);

        details::helpers::setNonBlocking(m_dataFd);

        m_dataConnectionAddress.sin_port = 0;
        do
        {
            systemOperationRes = bind(
                    m_dataFd, reinterpret_cast<sockaddr *>(&m_dataConnectionAddress), sizeof(m_dataConnectionAddress));
        } while (systemOperationRes < 0);

        systemOperationRes = 0;
        do
        {
            systemOperationRes = listen(m_dataFd, 15);
        } while (systemOperationRes < 0);

        getsockname(m_dataFd, reinterpret_cast<sockaddr *>(&m_dataConnectionAddress), &addrLen);
    }
    std::uint32_t dataIp = ntohl(m_dataConnectionAddress.sin_addr.s_addr);
    std::uint16_t dataPort = ntohs(m_dataConnectionAddress.sin_port);

    std::stringstream replyStr;

    replyStr << "227 Entering passive mode ("
             << (dataIp >> 24  & 0xFF) << ","
             << (dataIp >> 16  & 0xFF) << ","
             << (dataIp >> 8 & 0xFF) << ","
             << (dataIp & 0xFF) << ","
             << (dataPort >> 8  & 0xFF) << ","
             << (dataPort & 0xFF) << ")";

    reply(replyStr.str());
}

void Connection::list(const std::filesystem::path &path)
{
    if(
            path.lexically_normal() != m_root.parent_path()
            || !exists(path)
            || is_other(path)
            || is_symlink(path))
    {
        reply("534 Request denied");
        return;
    }
    m_file = popen(("ls -l " + (path).string()).c_str(), "r");
    sendFile(fileno(m_file));
}

void Connection::processNewCommand()
{
    std::size_t spaceLocation = m_msg.find(' ');
    std::size_t eolLocation = m_msg.find("\r\n");
    details::ci_string command(
            m_msg.substr(
                    0, spaceLocation != std::string::npos && spaceLocation < eolLocation
                       ? spaceLocation
                       : eolLocation).c_str());
    std::string argument;
    if (spaceLocation != std::string::npos)
        argument = m_msg.substr(spaceLocation + 1, eolLocation - spaceLocation - 1);
    // Теперь в command хранится case-insensitive строка с телом команды
    // (3 или 4 символа перед пробелом или переводом каретки)
    // из m_msg необходимо удалить тело команды, так как оно уже разобрано на command и argument.
    // при этом m_msg нельзя очистить полностью, так как после совпадения может оставаться
    // "хвост" из успевшей дойти части сообщения.
    m_msg.erase(0, eolLocation + 2);
    if (command == "USER")
    {
        if (argument.empty())
        {
            reply("501 Please, specify a username");
            return;
        }
        user(argument);
    }
    else if (command == "QUIT")
    {
        quit();
    }
    else if (command == "NOOP")
    {
        noop();
    }
    else if (!m_isAuthenticated)
    {
        reply("530 Not logged in");
    }
    else if (m_isAuthenticated)
    {
        if (command == "TYPE")
        {
            auto secondSpaceLocation = argument.find(' ', spaceLocation + 1);
            RepresentationType typeVal;
            Format formatVal = Format::N;
            if (secondSpaceLocation == std::string::npos && argument.size() == 1)
            {
                if ("AEIL"s.find(argument[0]) != std::string::npos)
                    typeVal = static_cast<RepresentationType>(argument[0]);
                else
                {
                    reply("501 Invalid argument");
                    return;
                }
                type(typeVal);
            }
            else if (argument.size() == 3 && secondSpaceLocation == 1)
            {
                if ("AEIL"s.find(argument[0]) != std::string::npos && "NTC"s.find(argument[2]) != std::string::npos)
                {
                    typeVal = static_cast<RepresentationType>(argument[0]);
                    formatVal = static_cast<Format>(argument[2]);
                }
                else
                {
                    reply("501 Invalid arguments");
                    return;
                }
                type(typeVal, formatVal);
            }
        }
        else if (command == "MODE")
        {
            if (argument.size() != 1)
            {
                reply("501 Please, specify the mode");
                return;
            }
            if ("SBC"s.find(argument[0]) != std::string::npos)
                mode(static_cast<Mode>(argument[0]));
            else
                reply("501 Invalid mode");
        }
        else if (command == "STRU")
        {
            if (argument.size() != 1)
            {
                reply("501 Please, specify the mode");
                return;
            }
            if ("FRP"s.find(argument[0]) != std::string::npos)
                stru(static_cast<Structure>(argument[0]));
            else
                reply("501 Invalid structure");
        }
        else if (command == "RETR" || command == "STOR" || command == "LIST")
        {
            if (argument.empty())
            {
                if(command != "LIST")
                {
                    reply("501 Please, specify the path");
                    return;
                }
                list(m_root.parent_path());
                return;
            }
            std::filesystem::path desiredPath;
            try
            {
                desiredPath = details::helpers::validatePath(argument);
            } catch (...)
            {
                reply("501 Invalid path");
                return;
            }
            if(!desiredPath.has_filename())
                desiredPath = desiredPath.parent_path();
            if (command == "RETR")
                retr(m_root/desiredPath);
            else if (command == "STOR")
                stor(m_root/desiredPath);
            else if (command == "LIST")
                list(m_root/desiredPath);
        }
        else if (command == "PASV")
        {
            pasv();
        }
        else if (command == "PWD")
        {
            pwd();
        }
        else
        {
            reply("500 Unknown command");
        }
    }
    else
    {
        reply("500 Unknown command");
    }
}

void Connection::reply(const std::string &reply)
{
    m_messageEngine->async_write(m_fd, reply + "\r\n", m_defaultBehavior);
}

void Connection::echo(const std::string &msg)
{
    auto aliveCriteria = m_isAlive;
    m_messageEngine->async_write(
            m_fd,
            msg + "\r\n",
            std::make_shared<messaging::CallbackType>(
                    [this, aliveCriteria](int res)
                    {
                        if(aliveCriteria->load())
                            if(res < 0)
                                killSelf();
                    }));
}

void Connection::sendFile(int fileFd)
{
    auto aliveCriteria = m_isAlive;
    m_messageEngine->async_write(
            m_fd,
            "150 Opening data connection\r\n"s,
            std::make_shared<messaging::CallbackType>(
                    [this, aliveCriteria](int res)
                    {
                        if(aliveCriteria->load())
                        {
                            if (res > 0)
                            {
                                m_messageEngine->async_accept(
                                        m_dataFd, std::make_shared<messaging::CallbackType>(
                                                [this, aliveCriteria](int res)
                                                {
                                                    if(aliveCriteria->load())
                                                    {
                                                        if (res < 0)
                                                        {
                                                            // Если accept не удался, сообщаем об ошибке и ждем следующей команды
                                                            reply("425 Cannot open data connection");
                                                            return;
                                                        }
                                                        // Если accept удался, значит, соединение открыто и можно передавать данные в полученный сокет
                                                        m_dataTransmissionFd = res;
                                                        // Запрашиваем передачу данных по имеющимся дескрипторам
                                                        (*m_repeatedDataSender)(0);
                                                    }
                                                }));
                            }
                            else
                            {
                                killSelf();
                            }
                        }
                    }));

}

void Connection::recvFile(int fileFd)
{
    auto aliveCriteria = m_isAlive;
    echo("150 Opening data connection");
    m_messageEngine->async_accept(
            m_dataFd,
            std::make_shared<messaging::CallbackType>(
                    [this, aliveCriteria](int res)
                    {
                        if(aliveCriteria->load())
                        {
                            if (res < 0)
                            {
                                // Если accept не удался, сообщаем об ошибке и ждем следующей команды
                                reply("425 Cannot open data connection");
                                return;
                            }
                            // Если accept удался, значит, соединение открыто и можно передавать данные в полученный сокет
                            m_dataTransmissionFd = res;
                            // Запрашиваем передачу данных по имеющимся дескрипторам
                            (*m_repeatedDataReceiver)(0);
                        }
                    }));
}

}