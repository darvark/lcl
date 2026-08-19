cmake_minimum_required(VERSION 3.10)

project(logger C CXX)

if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)
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

include_directories(src)

add_executable(logger
    qt_frontend.cpp
    app_controller.c
    db.c
    suggestion.c
    qso.c
    qtc.c
    config.c
    cty.c
    contest.c
    cat.c
    cw_keys.c
    dxcluster.c
    export.c
    stats.c
    maidenhead.c
)

target_include_directories(logger
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

if (HAMLIB_FOUND)
    target_compile_definitions(logger PRIVATE HAVE_HAMLIB=1)
    target_include_directories(logger PRIVATE ${HAMLIB_INCLUDE_DIRS})
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

if (HAMLIB_FOUND)
    target_link_libraries(logger ${HAMLIB_LIBRARIES})
endif()

add_executable(regression_tests
    tests/regression/regression_tests.c
    suggestion.c
    config.c
    cty.c
    contest.c
    qso.c
    qtc.c
    db.c
    export.c
    stats.c
    maidenhead.c
)

target_include_directories(regression_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(regression_tests
    m
    ${SQLITE3_LIBRARY}
)

target_include_directories(regression_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME regression_tests COMMAND regression_tests)

add_executable(unit_tests
    tests/unit/unit_tests.c
    app_controller.c
    cat.c
    cw_keys.c
    db.c
    suggestion.c
    config.c
    contest.c
    cty.c
    qso.c
    qtc.c
    export.c
    stats.c
    maidenhead.c
    dxcluster.c
)

target_include_directories(unit_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(unit_tests
    pthread
    m
    ${SQLITE3_LIBRARY}
)

target_include_directories(unit_tests
    PRIVATE
    ${CMAKE_SOURCE_DIR}
)

add_test(NAME unit_tests COMMAND unit_tests)

configure_file(${CMAKE_SOURCE_DIR}/cw_keys.ini cw_keys.ini COPYONLY)
