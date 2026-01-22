cmake_minimum_required(VERSION 3.16)
project(dailyarb VERSION 1.0.0 LANGUAGES CXX)

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

# FetchContent for dependencies
include(FetchContent)

# Boost.Beast for WebSocket (header-only)
FetchContent_Declare(
    boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0-cmake.tar.xz
    URL_HASH SHA256=2ab0a5e08cc8b6236f3c3608e3f0a4ae3c5e8e0f6f39d82dcf63e6ea8b671d89
)

# nlohmann/json for JSON parsing
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)

# fmt for formatting (must come before spdlog)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
)

# Make fmt available first
FetchContent_MakeAvailable(fmt)

# Configure spdlog to use external fmt (avoid bundled fmt conflicts)
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
    src/market_data/binance_client.cpp
    src/market_data/polymarket_client.cpp
    src/market_data/order_book.cpp
    src/strategy/strategy_base.cpp
    src/strategy/underpricing_strategy.cpp
    src/strategy/stale_odds_strategy.cpp
    src/execution/execution_engine.cpp
    src/execution/order.cpp
    src/risk/risk_manager.cpp
    src/position/position_manager.cpp
    src/ui/terminal_ui.cpp
    src/utils/crypto.cpp
    src/utils/time_utils.cpp
    src/utils/metrics.cpp
    src/persistence/trade_ledger.cpp
)

# Create static library
add_library(arblib STATIC ${LIB_SOURCES})

target_link_libraries(arblib PUBLIC
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    ${CURL_LIBRARIES}
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
)

# Check for ncurses
find_package(Curses)
if(CURSES_FOUND)
    target_compile_definitions(arblib PUBLIC HAS_NCURSES)
    target_include_directories(arblib PUBLIC ${CURSES_INCLUDE_DIRS})
    target_link_libraries(arblib PUBLIC ${CURSES_LIBRARIES})
endif()

# Main executable
add_executable(dailyarb src/main.cpp)
target_link_libraries(dailyarb PRIVATE
    arblib
    CLI11::CLI11
)

# Tests
enable_testing()
add_executable(tests
    tests/test_main.cpp
    tests/test_underpricing.cpp
    tests/test_risk_manager.cpp
    tests/test_order_book.cpp
    tests/test_fee_calculation.cpp
)
target_link_libraries(tests PRIVATE
    arblib
    GTest::gtest
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(tests)

# Replay tool for backtesting
add_executable(replay_tool src/tools/replay_tool.cpp)
target_link_libraries(replay_tool PRIVATE
    arblib
    CLI11::CLI11
)

# Install targets
install(TARGETS dailyarb replay_tool
    RUNTIME DESTINATION bin
)
install(DIRECTORY configs/ DESTINATION etc/dailyarb)
