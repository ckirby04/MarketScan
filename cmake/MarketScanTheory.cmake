# ---------------------------------------------------------------------------
# MarketScanTheory.cmake
#
# Everything needed to build a theory plugin. Usable two ways:
#
#   In-tree     the root CMakeLists includes this module, and theories/*/
#               subdirectories call marketscan_add_theory().
#
#   Standalone  a theory copied out of the repository includes this file
#               directly and builds against the SDK headers, with no other
#               part of MarketScan present. That path is what makes the
#               theories/ folder a USB port rather than a subdirectory: a
#               plugin does not need the host's build to exist.
#
# A theory links *no* MarketScan library. The SDK is two headers -- the pure-C
# ABI and the C++ ergonomic layer over it -- and that is deliberate: linking
# the engine into a plugin would give it a second copy of the host's state.
# ---------------------------------------------------------------------------

if(NOT DEFINED MARKETSCAN_SDK_INCLUDE_DIR)
    get_filename_component(MARKETSCAN_SDK_INCLUDE_DIR
        "${CMAKE_CURRENT_LIST_DIR}/../include" ABSOLUTE)
endif()

if(NOT EXISTS "${MARKETSCAN_SDK_INCLUDE_DIR}/marketscan/abi/plugin_abi.h")
    message(FATAL_ERROR
        "MarketScan SDK headers not found under '${MARKETSCAN_SDK_INCLUDE_DIR}'.\n"
        "Set MARKETSCAN_SDK_INCLUDE_DIR to the directory containing "
        "marketscan/abi/plugin_abi.h before including MarketScanTheory.cmake.")
endif()

if(NOT TARGET marketscan::sdk)
    add_library(marketscan_sdk INTERFACE)
    add_library(marketscan::sdk ALIAS marketscan_sdk)
    target_include_directories(marketscan_sdk INTERFACE "${MARKETSCAN_SDK_INCLUDE_DIR}")
    target_compile_features(marketscan_sdk INTERFACE cxx_std_20)
    if(MSVC)
        # /Zc:preprocessor is required: MARKETSCAN_EXPORT_STRATEGY relies on a
        # conforming preprocessor. The rest keeps a plugin's build honest.
        target_compile_options(marketscan_sdk INTERFACE
            /W4 /permissive- /Zc:preprocessor /Zc:__cplusplus /utf-8 /EHsc)
    endif()
endif()

# ---------------------------------------------------------------------------
# marketscan_add_theory(<target>
#     SOURCES <file>...
#     [OUTPUT_DIRECTORY <dir>]   # default: <runtime>/theories
# )
#
# Produces <target>.dll with no "lib" prefix, in the directory the host scans.
# ---------------------------------------------------------------------------
function(marketscan_add_theory target)
    cmake_parse_arguments(THEORY "" "OUTPUT_DIRECTORY" "SOURCES;LINK_LIBRARIES" ${ARGN})

    if(NOT THEORY_SOURCES)
        message(FATAL_ERROR "marketscan_add_theory(${target}): SOURCES is required")
    endif()

    if(NOT THEORY_OUTPUT_DIRECTORY)
        if(DEFINED MARKETSCAN_RUNTIME_DIR)
            set(THEORY_OUTPUT_DIRECTORY "${MARKETSCAN_RUNTIME_DIR}/theories")
        else()
            set(THEORY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/theories")
        endif()
    endif()

    # MODULE, not SHARED: a theory is loaded with LoadLibrary and never linked
    # against, so it needs no import library.
    add_library(${target} MODULE ${THEORY_SOURCES})

    target_link_libraries(${target} PRIVATE marketscan::sdk ${THEORY_LINK_LIBRARIES})

    # Turns MS_EXPORT into dllexport. Without it the three entry points are
    # compiled but not exported, and the loader refuses the plugin with
    # "missing export(s)" -- which is the correct error, but a confusing one
    # to hit because of a build setting.
    target_compile_definitions(${target} PRIVATE MARKETSCAN_PLUGIN_BUILD=1)

    set_target_properties(${target} PROPERTIES
        PREFIX ""
        SUFFIX ".dll"
        LIBRARY_OUTPUT_DIRECTORY "${THEORY_OUTPUT_DIRECTORY}"
        FOLDER "theories")

    # Multi-config generators append the configuration name to the output
    # directory unless told otherwise. The host scans one directory.
    foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${target} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${THEORY_OUTPUT_DIRECTORY}")
    endforeach()
endfunction()
