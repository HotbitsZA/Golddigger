cmake_minimum_required(VERSION 3.16)
project(GolddiggerUI CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(GOLDDIGGER_UI_BUILD_TESTS "Build the GolddiggerUI tests" ON)

# Source/header extensions of the drogon project template. They have to be set
# before the first add_executable() call: CMake expands the variable there.
if(NOT DEFINED DEFAULT_SOURCE_EXTENSION)
    set(DEFAULT_SOURCE_EXTENSION "cc" CACHE STRING "Default source file extension (e.g., cc, cpp, cxx)")
endif()
if(NOT DEFINED DEFAULT_HEADER_EXTENSION)
    set(DEFAULT_HEADER_EXTENSION "h" CACHE STRING "Default header file extension (e.g., h, hpp, hxx)")
endif()

set(GOLDDIGGER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Root of the Golddigger project")

# ##############################################################################
# The Golddigger libraries the UI needs.
#
# When the UI is built as part of the parent project (the recommended way) the
# already built libraries are reused, so dlib, curl and ZeroMQ are only built
# once. Standalone builds compile the handful of sources the UI needs here.
# ##############################################################################
if(TARGET golddigger_prediction)
    set(GOLDDIGGER_UI_CORE golddigger_prediction)
    message(STATUS "GolddiggerUI: using the Golddigger targets of the parent project")
else()
    message(STATUS "GolddiggerUI: building the Golddigger sources standalone")

    # Homebrew hints, so the standalone build behaves like the parent one.
    if(NOT DEFINED GOLDDIGGER_BREW_PREFIX)
        execute_process(
            COMMAND brew --prefix
            RESULT_VARIABLE GOLDDIGGER_BREW_RESULT
            OUTPUT_VARIABLE GOLDDIGGER_BREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT GOLDDIGGER_BREW_RESULT EQUAL 0)
            set(GOLDDIGGER_BREW_PREFIX "")
        endif()
        set(GOLDDIGGER_BREW_PREFIX "${GOLDDIGGER_BREW_PREFIX}" CACHE INTERNAL "Homebrew prefix, if any")
    endif()
    set(GOLDDIGGER_BREW_OPT
        "${GOLDDIGGER_BREW_PREFIX}/opt"
        /opt/homebrew/opt
        /usr/local/opt
    )

    find_package(dlib REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED HINTS ${GOLDDIGGER_BREW_OPT}/nlohmann-json)
    find_package(CURL REQUIRED)
    find_package(Threads REQUIRED)

    set(THREAD_COMPONENT_DIR
        "${GOLDDIGGER_ROOT}/../Repository/Software/ThreadComponent"
        CACHE PATH "Path to cBaseWorker_V2.h and cHTTPClient.cpp")

    find_path(ZMQ_INCLUDE_DIR
        NAMES zmq.h
        HINTS ${GOLDDIGGER_BREW_OPT}/zeromq
        PATH_SUFFIXES include
    )
    find_library(ZMQ_LIBRARY
        NAMES zmq libzmq
        HINTS ${GOLDDIGGER_BREW_OPT}/zeromq
        PATH_SUFFIXES lib
    )

    if(NOT EXISTS "${THREAD_COMPONENT_DIR}/cBaseWorker_V2.h")
        message(FATAL_ERROR "cBaseWorker_V2.h was not found in ${THREAD_COMPONENT_DIR}")
    endif()
    if(NOT ZMQ_INCLUDE_DIR OR NOT ZMQ_LIBRARY)
        message(FATAL_ERROR "libzmq was not found. Install ZeroMQ before building GolddiggerUI.")
    endif()
    if(NOT EXISTS "${GOLDDIGGER_ROOT}/Features/feature_builder.cpp")
        message(FATAL_ERROR "GOLDDIGGER_ROOT does not look like the Golddigger project: ${GOLDDIGGER_ROOT}")
    endif()

    add_library(golddigger_ui_core STATIC
        "${GOLDDIGGER_ROOT}/Features/feature_builder.cpp"
        "${GOLDDIGGER_ROOT}/Indicators/indicators.cpp"
        "${GOLDDIGGER_ROOT}/Utils/utils.cpp"
        "${GOLDDIGGER_ROOT}/MarketData/candle_data_provider.cpp"
        "${GOLDDIGGER_ROOT}/MarketData/dukascopy_cli_provider.cpp"
        "${GOLDDIGGER_ROOT}/MarketData/alpha_vantage_fx_provider.cpp"
        "${GOLDDIGGER_ROOT}/MarketData/mt5_zeromq_provider.cpp"
        "${GOLDDIGGER_ROOT}/MT5/mt5_protocol.cpp"
        "${GOLDDIGGER_ROOT}/MT5/mt5_zeromq_client.cpp"
        "${THREAD_COMPONENT_DIR}/cHTTPClient.cpp"
    )

    target_include_directories(golddigger_ui_core
        PUBLIC
            "${GOLDDIGGER_ROOT}"
            "${THREAD_COMPONENT_DIR}"
            "${ZMQ_INCLUDE_DIR}"
    )

    target_link_libraries(golddigger_ui_core
        PUBLIC
            dlib::dlib
            nlohmann_json::nlohmann_json
            CURL::libcurl
            ${ZMQ_LIBRARY}
            Threads::Threads
    )

    target_compile_options(golddigger_ui_core PRIVATE -Wall -Wextra)

    set(GOLDDIGGER_UI_CORE golddigger_ui_core)
endif()

# ##############################################################################
# Drogon
#
# The imported drogon targets export their include directory as a SYSTEM
# include directory. On macOS a second copy of the drogon headers can live in
# /usr/local/include, and that one is searched before the system directories
# of an imported target. The inline templates of drogon (the route
# registration for example) are then compiled against a different version of
# the library than the one that is linked. The mismatch is silent: the routes
# never reach the router and every request is answered with a 404.
#
# CMAKE_NO_SYSTEM_FROM_IMPORTED puts the include directories of the imported
# targets on the regular include path, which is searched before the default
# system directories, so the headers of the library we link against win.
# ##############################################################################
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

get_target_property(GOLDDIGGER_DROGON_INCLUDE_DIRS
                    Drogon::Drogon INTERFACE_INCLUDE_DIRECTORIES)
if(GOLDDIGGER_DROGON_INCLUDE_DIRS)
    message(STATUS "GolddiggerUI: drogon headers from ${GOLDDIGGER_DROGON_INCLUDE_DIRS}")
endif()

# ##############################################################################
# The file backed services. They live in their own library so the tests can
# use them without starting a web server.
# ##############################################################################
aux_source_directory(services SRV_SRC)

add_library(golddigger_ui_services STATIC ${SRV_SRC})

target_include_directories(golddigger_ui_services
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(golddigger_ui_services
    PUBLIC
        ${GOLDDIGGER_UI_CORE}
        nlohmann_json::nlohmann_json
        Drogon::Drogon
)

target_compile_options(golddigger_ui_services PRIVATE -Wall -Wextra)

# Default locations of the data, models and static front end. All of them can
# be overridden at runtime (environment variables or command line options).
#
# They have to be set on the services library and not on the executable:
# AppPaths.cc lives in the library and is the one that reads them.
target_compile_definitions(golddigger_ui_services
    PUBLIC
        GOLDDIGGER_ROOT="${GOLDDIGGER_ROOT}"
        GOLDDIGGER_DATA_DIR="${GOLDDIGGER_ROOT}/Data"
        GOLDDIGGER_MODELS_DIR="${GOLDDIGGER_ROOT}/Models"
        GOLDDIGGER_UI_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/web"
        GOLDDIGGER_UI_STATE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/state"
)

# ##############################################################################
# The server
# ##############################################################################
aux_source_directory(controllers CTL_SRC)

add_executable(${PROJECT_NAME}
    main.${DEFAULT_SOURCE_EXTENSION}
    ${CTL_SRC}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        golddigger_ui_services
        Drogon::Drogon
)

if(GOLDDIGGER_DROGON_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
                               ${GOLDDIGGER_DROGON_INCLUDE_DIRS})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

# ##############################################################################
# Tests
# ##############################################################################
if(GOLDDIGGER_UI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
