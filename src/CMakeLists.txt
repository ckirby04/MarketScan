# ---------------------------------------------------------------------------
# Generated headers
# ---------------------------------------------------------------------------
set(MARKETSCAN_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated" CACHE INTERNAL "")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.hpp.in"
    "${MARKETSCAN_GENERATED_INCLUDE_DIR}/marketscan/util/version.hpp"
    @ONLY)

# ---------------------------------------------------------------------------
# Third party
# ---------------------------------------------------------------------------
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# marketscan_core
#
# The engine: types, clock, events, feeds, book, execution, metrics.
# Deliberately free of any GUI dependency so that the Python bindings and the
# headless test suite link exactly the code the desktop app runs.
# ---------------------------------------------------------------------------
add_library(marketscan_core STATIC
    marketscan/book/order_book.cpp
    marketscan/core/clock.cpp
    marketscan/core/engine.cpp
    marketscan/core/money.cpp
    marketscan/core/symbol_table.cpp
    marketscan/core/types.cpp
    marketscan/data/bar_store.cpp
    marketscan/data/csv_feed.cpp
    marketscan/data/daily_feed.cpp
    marketscan/data/synthetic_feed.cpp
    marketscan/exec/broker.cpp
    marketscan/exec/fill_model.cpp
    marketscan/exec/ledger.cpp
    marketscan/metrics/correlation.cpp
    marketscan/metrics/performance.cpp
    marketscan/metrics/rolling.cpp
    marketscan/run/backtest.cpp
    marketscan/strategy/host_context.cpp
    marketscan/strategy/plugin_loader.cpp
    marketscan/strategy/registry.cpp
    marketscan/strategy/strategy_instance.cpp
    marketscan/util/digest.cpp
    marketscan/util/latency_histogram.cpp
    marketscan/util/logging.cpp
    marketscan/util/version.cpp)
add_library(marketscan::core ALIAS marketscan_core)

target_include_directories(marketscan_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${MARKETSCAN_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(marketscan_core
    PRIVATE marketscan::flags
    PUBLIC  spdlog::spdlog nlohmann_json::nlohmann_json)

target_compile_definitions(marketscan_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties(marketscan_core PROPERTIES FOLDER "marketscan")
