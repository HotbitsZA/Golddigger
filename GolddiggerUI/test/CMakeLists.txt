cmake_minimum_required(VERSION 3.16)
project(GolddiggerUI_test CXX)

# The controller is compiled into the test binary as well: the HTTP tests need
# the exact routes the server registers. aux_source_directory() hands out
# paths relative to the parent directory, so they are made absolute here.
set(CTL_SRC_ABS)
foreach(source IN LISTS CTL_SRC)
    list(APPEND CTL_SRC_ABS "${CMAKE_CURRENT_SOURCE_DIR}/../${source}")
endforeach()

add_executable(${PROJECT_NAME}
    test_main.cc
    test_env.cc
    services_test.cc
    api_test.cc
    ${CTL_SRC_ABS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE Drogon::Drogon golddigger_ui_services)

# See the comment in the parent CMakeLists.txt: the drogon headers have to be
# the ones of the library that is linked, otherwise the routes are lost.
if(GOLDDIGGER_DROGON_INCLUDE_DIRS)
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE
                               ${GOLDDIGGER_DROGON_INCLUDE_DIRS})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/..
                           ${CMAKE_CURRENT_SOURCE_DIR}/../services)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

enable_testing()
ParseAndAddDrogonTests(${PROJECT_NAME})
