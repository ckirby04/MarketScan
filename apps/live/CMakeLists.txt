# ---------------------------------------------------------------------------
# ms_live -- the command line front end to the live execution seam.
#
# Separate from the `marketscan` executable on purpose. The dashboard is a
# research tool that reads files; this one opens sockets and sends orders, and
# keeping them apart means the question "can this binary reach the broker" has
# a one-word answer for each of them.
#
# Every subcommand needs credentials from the environment and every one of them
# is paper-only by construction -- see include/marketscan/live/endpoints.hpp.
# ---------------------------------------------------------------------------
add_executable(ms_live
    ms_live.cpp
    equivalence.cpp
    dryrun_cmd.cpp
    equivalence_cmd.cpp
    orders_cmd.cpp
    positions_cmd.cpp
    probe_cmd.cpp)

target_link_libraries(ms_live PRIVATE marketscan::core marketscan::flags)

set_target_properties(ms_live PROPERTIES
    OUTPUT_NAME "ms_live"
    FOLDER "marketscan")
