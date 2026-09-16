# ---------------------------------------------------------------------------
# marketscan -- the application.
#
# With MARKETSCAN_BUILD_GUI the executable is the ImGui dashboard; without it
# the same binary is a headless runner, so CI can exercise the engine without
# pulling in the desktop dependency stack. There is one engine behind both.
# ---------------------------------------------------------------------------
add_executable(marketscan
    main.cpp
    headless.cpp)

target_link_libraries(marketscan PRIVATE marketscan::core marketscan::flags)

set_target_properties(marketscan PROPERTIES
    OUTPUT_NAME "marketscan"
    FOLDER "marketscan")

if(MARKETSCAN_BUILD_GUI)
    target_compile_definitions(marketscan PRIVATE MARKETSCAN_HAS_GUI=1)
else()
    target_compile_definitions(marketscan PRIVATE MARKETSCAN_HAS_GUI=0)
endif()
