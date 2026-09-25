cmake_minimum_required(VERSION 3.16)
project(LinuxXterm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Отключаем предупреждения об устаревших функциях VTE 
add_compile_options(-Wno-deprecated-declarations)

# Qt6 зависимости
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

# Поиск QTermWidget6 через pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(QTERMWIDGET REQUIRED qtermwidget6)

# Поиск libssh2 для SSH-сессии и SFTP
pkg_check_modules(LIBSSH2 REQUIRED libssh2)

# Пути к заголовочным файлам
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${QTERMWIDGET_INCLUDE_DIRS}
    ${LIBSSH2_INCLUDE_DIRS}
)

# Исходные файлы
set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/connection.cpp
    src/connectionmanager.cpp
    src/crypto.cpp
    src/connectiondialog.cpp
    src/sshsession.cpp
    src/logger.cpp
    src/passwordmanager.cpp
    src/masterpassworddialog.cpp
    src/customtermwidget.cpp
    src/sftpmanager.cpp
)

# Заголовочные файлы
set(HEADERS
    include/mainwindow.h
    include/connection.h
    include/connectionmanager.h
    include/crypto.h
    include/connectiondialog.h
    include/sshsession.h
    include/logger.h
    include/passwordmanager.h
    include/masterpassworddialog.h
    include/customtermwidget.h
    include/sftpmanager.h
)

add_executable(linuxxterm ${SOURCES} ${HEADERS})

# Подключение библиотек
target_link_libraries(linuxxterm PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    ${QTERMWIDGET_LIBRARIES}
    ${LIBSSH2_LIBRARIES}
)

# Пути к библиотекам
link_directories(${QTERMWIDGET_LIBRARY_DIRS} ${LIBSSH2_LIBRARY_DIRS})