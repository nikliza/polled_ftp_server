cmake_minimum_required(VERSION 3.10)

project(ftp_server_poll)
add_executable(ftp_server_poll
        src/main.cpp
        src/PollMessageEngine.cpp
        include/PollMessageEngine.h
        src/FtpConnection.cpp
        include/FtpConnection.h
        src/FTPServer.cpp
        include/FTPServer.h)

find_package(Boost 1.78 REQUIRED COMPONENTS program_options)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(${PROJECT_NAME} Boost::boost Boost::program_options)
target_include_directories(${PROJECT_NAME} PRIVATE include/ ${BOOST_INCLUDE_DIR})
