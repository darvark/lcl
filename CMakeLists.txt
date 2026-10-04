cmake_minimum_required(VERSION 3.10)

project(logger C CXX)

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LOGGER_ENABLE_OPENSSL "Build optional TLS transport support" ON)

add_compile_definitions(
    LOGGER_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LOGGER_BUILD_DIR="${CMAKE_BINARY_DIR}"
)

enable_testing()

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
set(OpenSSL_FOUND FALSE)
if (LOGGER_ENABLE_OPENSSL)
    find_package(OpenSSL QUIET)
endif()
find_library(SQLITE3_LIBRARY NAMES sqlite3 sqlite3.so.0 PATHS /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu REQUIRED)

set(HAMLIB_FOUND FALSE)
if (PkgConfig_FOUND)
    pkg_check_modules(HAMLIB QUIET hamlib)
    if (HAMLIB_FOUND)
        message(STATUS "Hamlib found via pkg-config: ${HAMLIB_VERSION}")
    endif()
endif()

if (NOT HAMLIB_FOUND)
    find_path(HAMLIB_INCLUDE_DIR hamlib/rig.h)
    find_library(HAMLIB_LIBRARY NAMES hamlib)
    if (HAMLIB_INCLUDE_DIR AND HAMLIB_LIBRARY)
        set(HAMLIB_FOUND TRUE)
        set(HAMLIB_INCLUDE_DIRS ${HAMLIB_INCLUDE_DIR})
        set(HAMLIB_LIBRARIES ${HAMLIB_LIBRARY})
        message(STATUS "Hamlib found via fallback search")
    endif()
endif()

include_directories(${CMAKE_SOURCE_DIR}/src)

set(LOGGER_CORE_SOURCES
    src/app_controller.c
    src/db.c
    src/suggestion.c
    src/qso.c
    src/qtc.c
    src/config.c
    src/cty.c
    src/scp.c
    src/contest.c
    src/cat.c
    src/cw_keys.c
    src/dxcluster.c
    src/live_upload.c
    src/export.c
    src/stats.c
    src/maidenhead.c
    src/net_protocol.c
    src/net_sync.c
    src/net_server.c
    src/net_tls.c
)

add_executable(logger
    src/qt_frontend.cpp
    ${LOGGER_CORE_SOURCES}
)

add_executable(newlog_file_test
    tests/unit/newlog_file_test.c
    ${LOGGER_CORE_SOURCES}
)

add_executable(unit_tests
    tests/unit/unit_tests.c
    ${LOGGER_CORE_SOURCES}
)

add_executable(network_tests
    tests/unit/unit_tests.c
    ${LOGGER_CORE_SOURCES}
)
target_compile_definitions(network_tests PRIVATE LOGGER_NETWORK_TESTS_ONLY=1)

add_executable(multiprocess_sync_tests
    tests/unit/multiprocess_sync_test.c
    ${LOGGER_CORE_SOURCES}
)

target_include_directories(logger
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(newlog_file_test
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(unit_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(network_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_include_directories(multiprocess_sync_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

if (HAMLIB_FOUND)
    target_compile_definitions(logger PRIVATE HAVE_HAMLIB=1)
    target_include_directories(logger PRIVATE ${HAMLIB_INCLUDE_DIRS})
    target_compile_definitions(newlog_file_test PRIVATE HAVE_HAMLIB=1)
    target_include_directories(newlog_file_test PRIVATE ${HAMLIB_INCLUDE_DIRS})
    target_compile_definitions(unit_tests PRIVATE HAVE_HAMLIB=1)
    target_include_directories(unit_tests PRIVATE ${HAMLIB_INCLUDE_DIRS})
    target_compile_definitions(network_tests PRIVATE HAVE_HAMLIB=1)
    target_include_directories(network_tests PRIVATE ${HAMLIB_INCLUDE_DIRS})
    target_compile_definitions(multiprocess_sync_tests PRIVATE HAVE_HAMLIB=1)
    target_include_directories(multiprocess_sync_tests PRIVATE ${HAMLIB_INCLUDE_DIRS})
endif()

if (OpenSSL_FOUND)
    target_compile_definitions(logger PRIVATE HAVE_OPENSSL=1)
    target_include_directories(logger PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_compile_definitions(newlog_file_test PRIVATE HAVE_OPENSSL=1)
    target_include_directories(newlog_file_test PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_compile_definitions(unit_tests PRIVATE HAVE_OPENSSL=1)
    target_include_directories(unit_tests PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_compile_definitions(network_tests PRIVATE HAVE_OPENSSL=1)
    target_include_directories(network_tests PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_compile_definitions(multiprocess_sync_tests PRIVATE HAVE_OPENSSL=1)
    target_include_directories(multiprocess_sync_tests PRIVATE ${OPENSSL_INCLUDE_DIR})
endif()

set(_qt_package "")

find_package(Qt6 COMPONENTS Widgets QUIET)
if (Qt6_FOUND)
    set(_qt_package Qt6)
else()
    find_package(Qt5 COMPONENTS Widgets QUIET)
    if (Qt5_FOUND)
        set(_qt_package Qt5)
    endif()
endif()

if (NOT _qt_package)
    message(FATAL_ERROR "Qt Widgets not found. Logger now requires Qt.")
endif()

target_link_libraries(logger
    ${_qt_package}::Widgets
    pthread
    m
    ${SQLITE3_LIBRARY}
)

target_link_libraries(newlog_file_test
    pthread
    m
    ${SQLITE3_LIBRARY}
)

target_link_libraries(unit_tests
    pthread
    m
    ${SQLITE3_LIBRARY}
)

target_link_libraries(network_tests
    pthread
    m
    ${SQLITE3_LIBRARY}
)

target_link_libraries(multiprocess_sync_tests
    pthread
    m
    ${SQLITE3_LIBRARY}
)

if (HAMLIB_FOUND)
    target_link_libraries(logger ${HAMLIB_LIBRARIES})
    target_link_libraries(newlog_file_test ${HAMLIB_LIBRARIES})
    target_link_libraries(unit_tests ${HAMLIB_LIBRARIES})
    target_link_libraries(network_tests ${HAMLIB_LIBRARIES})
    target_link_libraries(multiprocess_sync_tests ${HAMLIB_LIBRARIES})
endif()

if (OpenSSL_FOUND)
    target_link_libraries(logger OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(newlog_file_test OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(unit_tests OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(network_tests OpenSSL::SSL OpenSSL::Crypto)
    target_link_libraries(multiprocess_sync_tests OpenSSL::SSL OpenSSL::Crypto)
endif()

add_test(NAME newlog_file_test COMMAND newlog_file_test)
add_test(NAME network_tests COMMAND network_tests)
set_tests_properties(network_tests PROPERTIES RUN_SERIAL TRUE)
add_test(NAME multiprocess_sync_tests COMMAND multiprocess_sync_tests)
set_tests_properties(multiprocess_sync_tests PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)

configure_file(${CMAKE_SOURCE_DIR}/src/cw_keys.ini cw_keys.ini COPYONLY)
