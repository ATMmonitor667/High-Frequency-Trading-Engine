cmake_minimum_required(VERSION 3.16)
project(HighFrequencyTradingEngine LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Standard / build type
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Core library: every component except application entry points (main()).
# Tests and apps link against this single target.
# ---------------------------------------------------------------------------
add_library(hft STATIC
  src/core/orderbook/limit_order_book.cpp
  src/core/matching_engine/matching_engine.cpp
  src/marketdata/decoders/pitch_decoder.cpp
  src/marketdata/feed_handler/feed_handler.cpp
  src/marketdata/book_builder/order_book_builder.cpp
  src/strategy/strategy_engine.cpp
  src/risk/risk_engine.cpp
  src/execution/order_manager/order_manager.cpp
  src/execution/fills_processor/fills_processor.cpp
  src/execution/order_gateway/order_gateway.cpp
  src/execution/twap_vwap/twap_vwap.cpp
  src/portfolio/position_tracker.cpp
  src/storage/tick_store.cpp
)
target_include_directories(hft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
add_executable(trading_engine apps/trading_engine/trading_engine.cpp)
target_link_libraries(trading_engine PRIVATE hft)

add_executable(exchange_sim apps/exchange_sim/exchange_sim.cpp)
target_link_libraries(exchange_sim PRIVATE hft)

add_executable(latency_bench benchmarks/latency_bench.cpp)
target_link_libraries(latency_bench PRIVATE hft)

# ---------------------------------------------------------------------------
# Tests (each *.cpp is a self-contained test with its own main()).
# ---------------------------------------------------------------------------
enable_testing()

set(HFT_UNIT_TESTS
  test_timing_fixed_point
  test_limit_order_book
  test_pitch_decoder
  test_feed_handler
  test_order_book_builder
  test_strategy_engine
  test_risk_engine
  test_order_manager
  test_fills_processor
  test_position_tracker
  test_tick_store
  test_twap_vwap
  test_matching_engine
  test_order_gateway
  test_market_maker
)

foreach(t ${HFT_UNIT_TESTS})
  add_executable(${t} tests/unit/${t}.cpp)
  target_link_libraries(${t} PRIVATE hft)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

add_executable(test_feed_to_book tests/integration/test_feed_to_book.cpp)
target_link_libraries(test_feed_to_book PRIVATE hft)
add_test(NAME test_feed_to_book COMMAND test_feed_to_book)
