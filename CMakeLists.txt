cmake_minimum_required(VERSION 3.5)

project(Golddigger LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
set(THREAD_COMPONENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Repository/Software/ThreadComponent" CACHE PATH "Path to cBaseWorker_V2.h")

find_package(dlib REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED
    HINTS
        /opt/homebrew/opt/nlohmann-json
        /opt/homebrew/Cellar/nlohmann-json/3.12.0
)
find_package(CURL REQUIRED)

if(NOT EXISTS "${THREAD_COMPONENT_DIR}/cBaseWorker_V2.h")
    message(FATAL_ERROR "cBaseWorker_V2.h was not found in ${THREAD_COMPONENT_DIR}")
endif()

add_library(golddigger_common
    Indicators/indicators.cpp
    Utils/process_signals.cpp
    Utils/utils.cpp
)

target_include_directories(golddigger_common
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
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

add_library(golddigger_market_data
    MarketData/alpha_vantage_fx_provider.cpp
    MarketData/candle_data_provider.cpp
    MarketData/dukascopy_cli_provider.cpp
)

target_include_directories(golddigger_market_data
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(golddigger_market_data
    PUBLIC
        golddigger_http
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
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${CMAKE_CURRENT_SOURCE_DIR}/tunner.bin"
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
