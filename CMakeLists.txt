cmake_minimum_required(VERSION 3.16)

project(Golddigger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Resolve the Homebrew prefix once (cached) so dependency hints stay portable
# across Intel and Apple Silicon Macs, and degrade gracefully on Linux.
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
find_package(nlohmann_json CONFIG REQUIRED
    HINTS
        ${GOLDDIGGER_BREW_OPT}/nlohmann-json
)
find_package(CURL REQUIRED)

option(GOLDDIGGER_BUILD_FULL "Build the full pipeline (workers, market data, MT5, CLI tools)" ON)
option(GOLDDIGGER_BUILD_TESTS "Build the unit tests" ON)
option(GOLDDIGGER_BUILD_UI "Build the Golddigger web UI (requires Drogon)" ON)

add_library(golddigger_common
    Features/feature_builder.cpp
    Indicators/indicators.cpp
    Utils/process_signals.cpp
    Utils/utils.cpp
)

target_include_directories(golddigger_common
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(golddigger_common
    PUBLIC
        dlib::dlib
)

if(GOLDDIGGER_BUILD_FULL)
    set(THREAD_COMPONENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Repository/Software/ThreadComponent" CACHE PATH "Path to cBaseWorker_V2.h")

    find_path(ZMQ_INCLUDE_DIR
        NAMES zmq.h
        HINTS
            ${GOLDDIGGER_BREW_OPT}/zeromq
        PATH_SUFFIXES
            include
    )
    find_library(ZMQ_LIBRARY
        NAMES zmq libzmq
        HINTS
            ${GOLDDIGGER_BREW_OPT}/zeromq
        PATH_SUFFIXES
            lib
    )

    if(NOT EXISTS "${THREAD_COMPONENT_DIR}/cBaseWorker_V2.h")
        message(FATAL_ERROR "cBaseWorker_V2.h was not found in ${THREAD_COMPONENT_DIR}")
    endif()

    if(NOT ZMQ_INCLUDE_DIR OR NOT ZMQ_LIBRARY)
        message(FATAL_ERROR "libzmq was not found. Install ZeroMQ headers and library before building MT5 integration.")
    endif()

    add_library(ZeroMQ::libzmq UNKNOWN IMPORTED)
    set_target_properties(ZeroMQ::libzmq PROPERTIES
        IMPORTED_LOCATION "${ZMQ_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZMQ_INCLUDE_DIR}"
    )

    add_library(golddigger_training
        Training/training_support.cpp
        Training/trainer_worker.cpp
        Training/tuner_worker.cpp
    )

    target_include_directories(golddigger_training
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${THREAD_COMPONENT_DIR}"
    )

    target_link_libraries(golddigger_training
        PUBLIC
            golddigger_common
            dlib::dlib
    )

    add_library(golddigger_http
        "${THREAD_COMPONENT_DIR}/cHTTPClient.cpp"
    )

    target_include_directories(golddigger_http
        PUBLIC
            "${THREAD_COMPONENT_DIR}"
    )

    target_link_libraries(golddigger_http
        PUBLIC
            CURL::libcurl
    )

    add_library(golddigger_mt5
        MT5/mt5_protocol.cpp
        MT5/mt5_zeromq_client.cpp
    )

    target_include_directories(golddigger_mt5
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(golddigger_mt5
        PUBLIC
            nlohmann_json::nlohmann_json
            ZeroMQ::libzmq
    )

    add_library(golddigger_market_data
        MarketData/alpha_vantage_fx_provider.cpp
        MarketData/candle_data_provider.cpp
        MarketData/dukascopy_cli_provider.cpp
        MarketData/mt5_zeromq_provider.cpp
    )

    target_include_directories(golddigger_market_data
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(golddigger_market_data
        PUBLIC
            golddigger_http
            golddigger_mt5
            nlohmann_json::nlohmann_json
    )

    add_library(golddigger_prediction
        Prediction/predictor_engine.cpp
    )

    target_include_directories(golddigger_prediction
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(golddigger_prediction
        PUBLIC
            golddigger_market_data
            golddigger_common
            dlib::dlib
    )

    add_library(golddigger_data_update
        DataUpdate/daily_csv_updater.cpp
    )

    target_include_directories(golddigger_data_update
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(golddigger_data_update
        PUBLIC
            golddigger_market_data
            golddigger_common
    )

    add_executable(trainer
        trainer.cpp
    )

    target_link_libraries(trainer
        PRIVATE
            golddigger_training
    )

    add_custom_command(TARGET trainer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:trainer>/Models"
    )

    add_executable(predictor
        predictor.cpp
    )

    target_link_libraries(predictor
        PRIVATE
            golddigger_prediction
    )

    add_custom_command(TARGET predictor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:predictor>/Models"
    )

    add_executable(tuner
        tuner.cpp
    )

    set_target_properties(tuner PROPERTIES
        OUTPUT_NAME "tuner.bin"
    )

    target_link_libraries(tuner
        PRIVATE
            golddigger_training
    )

    add_custom_command(TARGET tuner POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:tuner>/Models"
        COMMAND "${CMAKE_COMMAND}" -E rm -f "${CMAKE_BINARY_DIR}/tunner.bin"
    )

    add_executable(patcher
        patcher.cpp
    )

    set_target_properties(patcher PROPERTIES
        OUTPUT_NAME "patcher.bin"
    )

    target_link_libraries(patcher
        PRIVATE
            golddigger_data_update
    )

    add_executable(mt5_gateway
        mt5_gateway.cpp
    )

    target_link_libraries(mt5_gateway
        PRIVATE
            golddigger_market_data
    )

    add_executable(validate
        Validation/walk_forward.cpp
    )

    set_target_properties(validate PROPERTIES
        OUTPUT_NAME "validate.bin"
    )

    target_link_libraries(validate
        PRIVATE
            golddigger_training
    )
else()
    message(STATUS "GOLDDIGGER_BUILD_FULL=OFF: skipping workers, market data, MT5 and CLI tools (tests only).")
endif()

# The web cockpit. It reuses golddigger_prediction and the other libraries
# above, so build it from here whenever the full pipeline is available. Drogon
# is optional: without it the rest of Golddigger still builds.
if(GOLDDIGGER_BUILD_FULL AND GOLDDIGGER_BUILD_UI)
    find_package(Drogon CONFIG QUIET)
    if(TARGET Drogon::Drogon)
        message(STATUS "GolddiggerUI: building the web UI")
        add_subdirectory(GolddiggerUI)
    else()
        message(STATUS "GolddiggerUI: Drogon was not found, skipping the web UI")
    endif()
endif()

if(GOLDDIGGER_BUILD_TESTS)
    enable_testing()

    # Small shared library that keeps unit tests free of the network/thread-component
    # dependencies (curl, ZeroMQ, cBaseWorker_V2) so they run anywhere.
    add_library(golddigger_test_core STATIC
        MarketData/candle_data_provider.cpp
        DataUpdate/daily_csv_updater.cpp
        MT5/mt5_protocol.cpp
    )

    target_include_directories(golddigger_test_core
        PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}"
    )

    target_link_libraries(golddigger_test_core
        PUBLIC
            nlohmann_json::nlohmann_json
    )

    add_executable(golddigger_tests
        Tests/test_main.cpp
        Tests/test_indicators.cpp
        Tests/test_csv.cpp
        Tests/test_features.cpp
        Tests/test_date.cpp
        Tests/test_protocol.cpp
    )

    target_link_libraries(golddigger_tests
        PRIVATE
            golddigger_common
            golddigger_test_core
            dlib::dlib
    )

    add_test(NAME golddigger_tests COMMAND golddigger_tests)
endif()