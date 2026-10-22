cmake_minimum_required(VERSION 3.16)
project(LinuxXterm VERSION 1.0.3 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_compile_options(-Wno-deprecated-declarations)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

find_package(PkgConfig REQUIRED)
pkg_check_modules(QTERMWIDGET REQUIRED qtermwidget6)

pkg_check_modules(LIBSSH2 REQUIRED libssh2)

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${QTERMWIDGET_INCLUDE_DIRS}
    ${LIBSSH2_INCLUDE_DIRS}
)

set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/connection.cpp
    src/connectionmanager.cpp
    src/connectiondialog.cpp
    src/sshsession.cpp
    src/logger.cpp
    src/passwordmanager.cpp
    src/masterpassworddialog.cpp
    src/customtermwidget.cpp
    src/sftpmanager.cpp
    src/droptreewidget.cpp
    src/legacyssh.cpp
    src/sshpassproxy.cpp
)

set(HEADERS
    include/mainwindow.h
    include/connection.h
    include/connectionmanager.h
    include/connectiondialog.h
    include/sshsession.h
    include/logger.h
    include/passwordmanager.h
    include/masterpassworddialog.h
    include/customtermwidget.h
    include/sftpmanager.h
    include/droptreewidget.h
    include/legacyssh.h
    include/sshpassproxy.h
)

add_executable(linuxxterm ${SOURCES} ${HEADERS})

target_link_libraries(linuxxterm PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    OpenSSL::Crypto
    ${QTERMWIDGET_LIBRARIES}
    ${LIBSSH2_LIBRARIES}
)

link_directories(${QTERMWIDGET_LIBRARY_DIRS} ${LIBSSH2_LIBRARY_DIRS})

if(BUILD_TESTING)
    add_executable(passwordmanager_tests
        tests/passwordmanager_test.cpp
        src/connection.cpp
        src/connectionmanager.cpp
        src/passwordmanager.cpp
        src/sshpassproxy.cpp
        include/connection.h
        include/connectionmanager.h
        include/passwordmanager.h
    )
    target_include_directories(passwordmanager_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(passwordmanager_tests PRIVATE Qt6::Core Qt6::Network OpenSSL::Crypto)
    add_test(NAME passwordmanager_tests COMMAND passwordmanager_tests)
endif()