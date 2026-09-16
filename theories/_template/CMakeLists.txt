# ---------------------------------------------------------------------------
# A theory's build.
#
# This file works two ways. Inside the MarketScan tree the module is already
# included and MARKETSCAN_RUNTIME_DIR is set, so the first branch is skipped.
# Copied somewhere else, it configures on its own against the SDK headers:
#
#     cmake -S . -B build -DMARKETSCAN_ROOT=/path/to/MarketScan
#     cmake --build build
#
# That standalone path is the point. A theory is not a subdirectory of this
# project; it is a separate thing that happens to speak the same ABI.
# ---------------------------------------------------------------------------
if(NOT COMMAND marketscan_add_theory)
    cmake_minimum_required(VERSION 3.25)
    project(marketscan_theory_template LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)

    set(MARKETSCAN_ROOT "" CACHE PATH "Root of a MarketScan checkout or SDK")
    if(NOT MARKETSCAN_ROOT)
        # Default to this file's usual home, three directories up.
        get_filename_component(MARKETSCAN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    endif()

    if(NOT EXISTS "${MARKETSCAN_ROOT}/cmake/MarketScanTheory.cmake")
        message(FATAL_ERROR
            "Could not find MarketScanTheory.cmake under '${MARKETSCAN_ROOT}'.\n"
            "Pass -DMARKETSCAN_ROOT=<path to your MarketScan checkout>.")
    endif()

    include("${MARKETSCAN_ROOT}/cmake/MarketScanTheory.cmake")

    # A standalone build drops the DLL beside itself; copy it into the host's
    # theories/ directory and press Rescan.
    set(_template_output "${CMAKE_BINARY_DIR}")
else()
    # In-tree: built to prove it compiles, but kept out of the scanned folder.
    set(_template_output "${CMAKE_BINARY_DIR}/theory_template")
endif()

marketscan_add_theory(my_theory
    SOURCES my_theory.cpp
    OUTPUT_DIRECTORY "${_template_output}")
