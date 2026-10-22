# ---------------------------------------------------------------------------
# marketscan_app -- the application's engine-facing half.
#
# Split out from the executable so the test suite can drive it. Session is the
# most concurrency-sensitive code in the project -- a worker thread running the
# engine while the GUI reads snapshots -- and code like that should not be
# reachable only by launching a window and looking at it.
#
# Nothing in here includes ImGui.
# ---------------------------------------------------------------------------
add_library(marketscan_app STATIC
    session.cpp
    headless.cpp
    ui_state.cpp
    palette.cpp
    series_colors.cpp
    chart_model.cpp
    research_session.cpp
    research_view.cpp
    schema_form.cpp)
add_library(marketscan::app ALIAS marketscan_app)

target_include_directories(marketscan_app PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(marketscan_app
    PUBLIC marketscan::core
    PRIVATE marketscan::flags)
set_target_properties(marketscan_app PROPERTIES FOLDER "marketscan")

# ---------------------------------------------------------------------------
# marketscan -- the executable.
#
# With MARKETSCAN_BUILD_GUI it is the ImGui dashboard; without it the same
# binary is a headless runner, and --headless reaches that path from the GUI
# build. One engine behind both.
# ---------------------------------------------------------------------------
add_executable(marketscan main.cpp)

target_link_libraries(marketscan PRIVATE marketscan::app marketscan::flags)

set_target_properties(marketscan PROPERTIES
    OUTPUT_NAME "marketscan"
    FOLDER "marketscan")

if(MARKETSCAN_BUILD_GUI)
    find_package(imgui CONFIG REQUIRED)
    find_package(implot CONFIG REQUIRED)
    find_package(glfw3 CONFIG REQUIRED)
    find_package(OpenGL REQUIRED)

    target_sources(marketscan PRIVATE
        app.cpp
        screenshot.cpp
        theme.cpp
        panels/panels.cpp
        panels/format.cpp
        panels/leaderboard.cpp
        panels/equity_chart.cpp
        panels/overview.cpp
        panels/research.cpp
        panels/equity_curves.cpp
        panels/charts.cpp
        panels/tables.cpp
        panels/theory_detail.cpp
        panels/universe.cpp
        panels/plugin_manager.cpp
        panels/transport.cpp
        panels/latency.cpp
        panels/log_console.cpp)

    target_link_libraries(marketscan PRIVATE
        imgui::imgui
        implot::implot
        glfw
        OpenGL::GL)

    target_compile_definitions(marketscan PRIVATE MARKETSCAN_HAS_GUI=1)

    # Fonts are read at startup from a path relative to the executable, so a
    # build tree behaves the same way an installed copy would.
    add_custom_command(TARGET marketscan POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:marketscan>/assets"
        COMMENT "Copying assets beside the executable")
else()
    target_compile_definitions(marketscan PRIVATE MARKETSCAN_HAS_GUI=0)
endif()
