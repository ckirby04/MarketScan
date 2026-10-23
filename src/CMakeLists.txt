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
    marketscan/data/identity_probe.cpp
    marketscan/data/security_master.cpp
    marketscan/data/universe.cpp
    marketscan/data/synthetic_feed.cpp
    marketscan/exec/broker.cpp
    marketscan/exec/delisting.cpp
    marketscan/exec/fill_model.cpp
    marketscan/exec/ledger.cpp
    marketscan/metrics/correlation.cpp
    marketscan/metrics/performance.cpp
    marketscan/metrics/rolling.cpp
    marketscan/research/drawdown_entry.cpp
    marketscan/research/job.cpp
    marketscan/research/json_schema.cpp
    marketscan/research/rule_run.cpp
    marketscan/run/backtest.cpp
    marketscan/strategy/host_context.cpp
    marketscan/strategy/plugin_loader.cpp
    marketscan/strategy/registry.cpp
    marketscan/strategy/strategy_instance.cpp
    marketscan/util/digest.cpp
    marketscan/util/latency_histogram.cpp
    marketscan/util/logging.cpp
    marketscan/util/sha256.cpp
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

# ---------------------------------------------------------------------------
# marketscan_live
#
# The paper-only live execution seam: HTTPS, WebSocket, the stream parser, the
# live broker and feed, recording and replay. Its own library rather than part
# of the core, so that what a binary can reach is decided by what it links:
# ms_live links this; the desktop app, the research job and the Python
# bindings do not, and so cannot reach a broker however they are driven.
# SPEC_RESEARCH_VIEW.md section 2 asks that of the research path, and
# tests/test_research_job.cpp checks the built ms_research for it.
# ---------------------------------------------------------------------------
add_library(marketscan_live STATIC
    marketscan/live/calendar.cpp
    marketscan/live/credentials.cpp
    marketscan/live/http.cpp
    marketscan/live/live_broker.cpp
    marketscan/live/live_feed.cpp
    marketscan/live/order_map.cpp
    marketscan/live/order_tracker.cpp
    marketscan/live/recorded_broker.cpp
    marketscan/live/recorded_feed.cpp
    marketscan/live/recorder.cpp
    marketscan/live/stream_parser.cpp
    marketscan/live/trade_updates.cpp
    marketscan/live/websocket.cpp
)
add_library(marketscan::live ALIAS marketscan_live)

target_link_libraries(marketscan_live
    PRIVATE marketscan::flags
    PUBLIC  marketscan::core)

# WinHTTP, for the live execution layer's HTTPS and WebSocket. In the operating
# system rather than vendored: adding a dependency to a frozen-ABI project has a
# long tail -- a port to pin, a baseline to bump, a transitive TLS stack, a CI
# cache to grow -- and one OS API covers both halves.
if(WIN32)
    target_link_libraries(marketscan_live PRIVATE winhttp)
endif()

set_target_properties(marketscan_live PROPERTIES FOLDER "marketscan")

# ---------------------------------------------------------------------------
# marketscan_service
#
# The research path's one way onto the network: the job service, for the
# desktop Research view's conversation (docs/specs/SPEC_DESKTOP_CHAT.md). Its
# own library, like marketscan_live and for the same reason: what a binary can
# reach is decided by what it links. It does not link marketscan_live, so it
# cannot reach a broker, and ms_research links neither.
# ---------------------------------------------------------------------------
add_library(marketscan_service STATIC
    marketscan/research/service_client.cpp)
add_library(marketscan::service ALIAS marketscan_service)

target_link_libraries(marketscan_service
    PRIVATE marketscan::flags
    PUBLIC  marketscan::core)

if(WIN32)
    target_link_libraries(marketscan_service PRIVATE winhttp)
endif()

set_target_properties(marketscan_service PROPERTIES FOLDER "marketscan")
