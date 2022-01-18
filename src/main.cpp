#include <PollMessageEngine.h>

#include <iostream>
#include <fcntl.h>
#include <FTPServer.h>
#include <thread>
#include <boost/program_options.hpp>

void interruptionHandler(int signal)
{
    exit(0);
}

void checkAsyncIo()
{
    std::cout << "-------------- CheckAsyncIo() starts --------------\n";
    int fd1 = open("1.txt", O_WRONLY | O_CREAT, 0666),
        fd2 = open("1.txt", O_RDONLY | O_CREAT, 0666);
    messaging::PollMessageEngine engine;
    std::string msg1;
    msg1.resize(10);
    int eventCount = 0;
    engine.async_write(
            fd1,
            std::string("ABC\n"),
            std::make_shared<messaging::CallbackType>(
                    [&eventCount](int res) mutable
                    {
                        std::cout << "write successful\n";
                        ++eventCount;
                    }));

    engine.async_read(
            fd2,
            msg1,
            std::make_shared<messaging::CallbackType>(
                    [&msg1, &eventCount](int res) mutable
                    {
                        std::cout << "read successful\nmessage: " << msg1 << '\n';
                        ++eventCount;
                    }));

    engine.async_write(
            fd1,
            std::string("Hello\n"),
            std::make_shared<messaging::CallbackType>(
                    [&eventCount](int res)mutable
                    {
                        std::cout << "write successful\n";
                        ++eventCount;
                    }));
    while(eventCount < 3)
        engine.waitForEvent()();
    remove("1.txt");
}

void checkAsyncIoSome()
{
    std::cout << "------------ CheckAsyncIoSome() starts ------------\n";
    std::string msg1, msg2, msg3;
    int fd1 = open("1.txt", O_CREAT | O_RDONLY, 0666),
        fd2 = open("1.txt", O_CREAT | O_WRONLY, 0666);
    msg1 = "Hello!\n";
    msg2 = "My name is Liza\n";
    msg3.resize(msg1.size() + msg2.size());
    int eventCount = 0;
    messaging::PollMessageEngine engine;
    engine.async_write_some(
            fd2,
            msg1,
            std::make_shared<messaging::CallbackType>(
                    [&eventCount](int res) mutable
                    {
                        if(res > 0)
                            std::cout << "write successful\n";
                        else std::cerr << "write failed\n";
                      ++eventCount;
                    }));

    engine.async_read_some(
            fd1,
            msg3,
            std::make_shared<messaging::CallbackType>(
                    [&msg3, &eventCount](int res) mutable
                    {
                      if(res > 0)
                          std::cout << "Got a message: " << msg3 << '\n';
                      else
                          std::cerr << "Error reading a message\n";
                      ++eventCount;
                    }));

    engine.async_write_some(
            fd2,
            msg2,
            std::make_shared<messaging::CallbackType>(
                    [&eventCount](int res) mutable
                    {
                      if(res > 0)
                          std::cout << "write successful\n";
                      else std::cerr << "write failed\n";
                      ++eventCount;
                    }));

    engine.async_read_some(
            fd1,
            msg3,
            std::make_shared<messaging::CallbackType>(
                    [&msg3, &eventCount](int res) mutable
                    {
                        if(res > 0)
                            std::cout << "Got a message: " << msg3 << '\n';
                        else
                            std::cerr << "Error reading a message\n";
                        ++eventCount;
                    }));

    while(eventCount < 4)
        engine.waitForEvent()();
    remove("1.txt");
}

void checkAsyncIoUntil()
{
    int fd1 = open("1.txt", O_CREAT | O_RDONLY, 0666),
        fd2 = open("1.txt", O_CREAT | O_WRONLY, 0666);
    std::cout << "------------ CheckAsyncIoUntil() starts -----------\n";
    std::string msg;
    messaging::PollMessageEngine engine;
    int eventCount = 0;
    engine.async_write(
            fd2,
            std::string("Hello!\n"),
            std::make_shared<messaging::CallbackType>(
                [&eventCount](int res) mutable
                {
                    if(res > 0)
                        std::cout << "write successful\n";
                    else
                        std::cerr << "write failed\n";
                    ++eventCount;
                }));
    engine.async_write(
            fd2,
            std::string("My name is Liza\n"),
            std::make_shared<messaging::CallbackType>(
                    [&eventCount](int res) mutable
                    {
                        if(res > 0)
                            std::cout << "write successful\n";
                        else
                            std::cerr << "write failed\n";
                        ++eventCount;
                    }));
    engine.async_write(
            fd2,
            std::string("I am from Russia\n"),
            std::make_shared<messaging::CallbackType>(
                   [&eventCount](int res) mutable
                   {
                       if(res > 0)
                           std::cout << "write successful\n";
                       else
                           std::cerr << "write failed\n";
                       ++eventCount;
                   }));
    engine.async_read_until(
            fd1,
            msg,
            std::make_shared<messaging::CallbackType>(
                    [&eventCount, &msg](int res)
                    {
                        if(res > 0)
                            std::cout << "got a message: " << msg << '\n';
                        else
                            std::cerr << "read failed\n";
                        ++eventCount;
                    }), std::string("from"));
    while(eventCount < 4)
        engine.waitForEvent();
    remove("1.txt");
}

int main(int argc, const char* argv[])
{

    std::uint16_t port = -1;
    unsigned threadCount = -1;

    //Обработка параметров запуска программы
    boost::program_options::options_description desc("Allowed options");
    desc.add_options()
            ("help", "print this help message")
            ("threads", boost::program_options::value<unsigned>(&threadCount)->default_value(std::thread::hardware_concurrency()), "set the maximum cores to be used")
            ("port", boost::program_options::value<std::uint16_t>(&port), "set the port for the control connections");

    boost::program_options::variables_map options;

    try
    {
        boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), options);
        boost::program_options::notify(options);
    } catch(boost::program_options::unknown_option& opt)
    {
        std::cerr << "Unsupported options. Use \"--help\" option to view the list of available options\n";
    }

    if(options.count("help")){
        std::cout << desc << '\n';
        return 1;
    }

    if(port == std::uint16_t(-1) || threadCount == -1)
    {
        std::cerr << "Please, specify the launch options to use this program.\nTo find the options list, use \"--help\" option.\n";
        return 2;
    }

    //Назначаем обработчик сигналов для корректного завершения программы по прерыванию
    struct sigaction actionHandler;
    actionHandler.sa_handler = interruptionHandler;
    actionHandler.sa_flags = 0;
    sigaction(SIGINT, &actionHandler, nullptr);
    sigaction(SIGHUP, &actionHandler, nullptr);
    sigaction(SIGTSTP, &actionHandler, nullptr);
    sigaction(SIGTERM, &actionHandler, nullptr);

    //Создаем сокет, на котором будет работать сервер
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0)
    {
        std::cerr << "Socket() error: " << std::system_error(errno, std::system_category()).what() << '\n';
        return 1;
    }
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &(addr.sin_addr));
    addr.sin_port = htons(port);
    socklen_t addrLen = sizeof addr;

    if(bind(fd, reinterpret_cast<sockaddr*>(&addr), addrLen) < 0)
    {
        std::cerr << "Bind() error: " << std::system_error(errno, std::system_category()).what() << '\n';
        return 2;
    }

    listen(fd, 15);

    ftp::details::helpers::setNonBlocking(fd);

    // Получаем адрес сокета, чтобы сообщить его пользователю
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    std::string addrString(INET_ADDRSTRLEN, 0);
    inet_ntop(AF_INET, &(addr.sin_addr), addrString.data(), INET_ADDRSTRLEN);
    std::cout << "Address: " << addrString << '\n'
                << "Port: " << htons(addr.sin_port) << '\n';
    ftp::Server srv(fd, (std::filesystem::current_path()/"FTP/").lexically_normal(), threadCount);

    // Запуск сервера
    srv.start();
    char c;
    std::cout << "Введите любой символ для остановки сервера\n";
    std::cin >> c;
    srv.stop();
    return 0;
}
