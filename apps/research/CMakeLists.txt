# ---------------------------------------------------------------------------
# ms_research -- the research job, headless.
#
# Separate from `marketscan` and `ms_live` for the same reason those two are
# separate from each other: what a binary can do should have a short answer.
# This one reads a form and bars and writes a Results document. It submits no
# orders to anything but the simulator.
# ---------------------------------------------------------------------------
add_executable(ms_research ms_research.cpp)

target_link_libraries(ms_research PRIVATE marketscan::core marketscan::flags)

set_target_properties(ms_research PROPERTIES
    OUTPUT_NAME "ms_research"
    FOLDER "marketscan")
