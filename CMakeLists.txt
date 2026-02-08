cmake_minimum_required(VERSION 3.16)
project(delta_neutral_bot VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Find required packages
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)

# FetchContent for dependencies
include(FetchContent)

# nlohmann/json for JSON parsing
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

# fmt for formatting (must come before spdlog)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
)

# Make fmt available first
FetchContent_MakeAvailable(fmt)

# Configure spdlog to use external fmt
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use external fmt library" FORCE)

# spdlog for logging
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
)

# CLI11 for command line parsing
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.1
)

# Google Test for unit tests
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

FetchContent_MakeAvailable(json spdlog cli11 googletest)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

# Main library sources
set(LIB_SOURCES
    src/config/config.cpp
    src/exchange/binance_common.cpp
    src/exchange/binance_spot.cpp
    src/exchange/binance_futures.cpp
    src/exchange/hyperliquid_signer.cpp
    src/exchange/hyperliquid_client.cpp
    src/exchange/hyperliquid_perp.cpp
    src/utils/keccak256.cpp
    src/utils/msgpack_lite.cpp
    src/strategy/funding_monitor.cpp
    src/strategy/basis_calculator.cpp
    src/strategy/delta_neutral_engine.cpp
    src/execution/execution_engine.cpp
    src/execution/hedge_manager.cpp
    src/position/position_manager.cpp
    src/risk/risk_manager.cpp
    src/ui/terminal_ui.cpp
    src/utils/crypto.cpp
    src/utils/time_utils.cpp
    src/utils/metrics.cpp
    src/persistence/session_database.cpp
)

# Create static library
add_library(arblib STATIC ${LIB_SOURCES})

target_link_libraries(arblib PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ${CURL_LIBRARIES}
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
)

# Main executable
add_executable(delta_neutral_bot src/main.cpp)
target_link_libraries(delta_neutral_bot PRIVATE
    arblib
    CLI11::CLI11
)

# Backtester executable
add_executable(backtest_tool
    src/backtest/backtester.cpp
    src/backtest/main_backtest.cpp
)
target_link_libraries(backtest_tool PRIVATE
    arblib
    CLI11::CLI11
)

# Tests
enable_testing()
add_executable(tests
    tests/test_main.cpp
    tests/test_risk_manager.cpp
    tests/test_types.cpp
    tests/test_basis_calculator.cpp
    tests/test_position_manager.cpp
    tests/test_session_database.cpp
    tests/test_hyperliquid.cpp
)
target_link_libraries(tests PRIVATE
    arblib
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(tests)

# Install targets
install(TARGETS delta_neutral_bot
    RUNTIME DESTINATION bin
)
install(DIRECTORY configs/ DESTINATION etc/delta_neutral_bot)
