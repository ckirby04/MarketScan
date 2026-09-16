cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# vcpkg toolchain discovery.
# Must run before project(). Honours an explicitly supplied toolchain first,
# then VCPKG_ROOT, then a handful of conventional locations, so a fresh clone
# configures without the developer having to export anything.
# ---------------------------------------------------------------------------
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT DEFINED ENV{CMAKE_TOOLCHAIN_FILE})
    set(_ms_vcpkg_candidates
        "$ENV{VCPKG_ROOT}"
        "$ENV{VCPKG_INSTALLATION_ROOT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg"
        "$ENV{USERPROFILE}/vcpkg"
        "$ENV{HOME}/vcpkg"
        "C:/vcpkg")
    foreach(_ms_candidate IN LISTS _ms_vcpkg_candidates)
        if(_ms_candidate)
            file(TO_CMAKE_PATH "${_ms_candidate}" _ms_candidate)
            # A manifest build needs a real ports tree. This check rejects the
            # cut-down vcpkg bundle that ships inside Visual Studio, which
            # vcvars64.bat helpfully points VCPKG_ROOT at and which cannot
            # resolve a single dependency.
            if(EXISTS "${_ms_candidate}/scripts/buildsystems/vcpkg.cmake"
               AND IS_DIRECTORY "${_ms_candidate}/ports")
                set(CMAKE_TOOLCHAIN_FILE "${_ms_candidate}/scripts/buildsystems/vcpkg.cmake"
                    CACHE FILEPATH "vcpkg toolchain file")
                message(STATUS "MarketScan: using vcpkg at ${_ms_candidate}")
                break()
            endif()
        endif()
    endforeach()
    unset(_ms_candidate)
    unset(_ms_vcpkg_candidates)
endif()

project(MarketScan
    VERSION 0.1.0
    DESCRIPTION "Quantitative trading research platform"
    HOMEPAGE_URL "https://github.com/ckirby04/MarketScan"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Language level
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MARKETSCAN_BUILD_GUI      "Build the ImGui desktop application"  ON)
option(MARKETSCAN_BUILD_TESTS    "Build the Catch2 test suite"          ON)
option(MARKETSCAN_BUILD_THEORIES "Build the reference theory plugins"   ON)
option(MARKETSCAN_BUILD_PYTHON   "Build the pybind11 module"            OFF)
option(MARKETSCAN_WERROR         "Treat compiler warnings as errors"    OFF)

# ---------------------------------------------------------------------------
# Single output tree.
#
# Everything the app needs at runtime lands in <build>/bin so that the
# executable, its dependent DLLs and the theories/ plugin directory keep the
# same relative layout as an installed copy. Plugins are redirected into
# bin/theories by the theory helper in cmake/MarketScanTheory.cmake.
# ---------------------------------------------------------------------------
set(MARKETSCAN_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MARKETSCAN_RUNTIME_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${MARKETSCAN_RUNTIME_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${MARKETSCAN_RUNTIME_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${MARKETSCAN_RUNTIME_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/lib")
endforeach()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# ---------------------------------------------------------------------------
# Warning / codegen flags, applied through an interface target so that third
# party headers pulled in by vcpkg are never compiled with our settings.
# ---------------------------------------------------------------------------
add_library(marketscan_flags INTERFACE)
add_library(marketscan::flags ALIAS marketscan_flags)

if(MSVC)
    target_compile_options(marketscan_flags INTERFACE
        /W4                 # high warning level
        /permissive-        # standards conformance
        /Zc:preprocessor    # conforming preprocessor, required by the ABI macros
        /Zc:__cplusplus     # report the real __cplusplus value
        /utf-8              # source and execution charset
        /EHsc               # C++ exceptions, extern "C" assumed nothrow
        /MP                 # parallel compilation
        /bigobj)
    target_compile_definitions(marketscan_flags INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_CXX20_CISO646_REMOVED_WARNING)
    if(MARKETSCAN_WERROR)
        target_compile_options(marketscan_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(marketscan_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-unused-parameter)
    if(MARKETSCAN_WERROR)
        target_compile_options(marketscan_flags INTERFACE -Werror)
    endif()
endif()

# Before any add_subdirectory: add_test() in a subdirectory only registers if
# testing was enabled at the top level first, and a test that silently fails to
# register is a test that silently stops running.
if(MARKETSCAN_BUILD_TESTS)
    enable_testing()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Defines marketscan::sdk and marketscan_add_theory(). The same file a theory
# built outside this tree includes.
include(MarketScanTheory)

# ---------------------------------------------------------------------------
# Subprojects
# ---------------------------------------------------------------------------
add_subdirectory(src)
add_subdirectory(apps/marketscan)

if(MARKETSCAN_BUILD_THEORIES)
    add_subdirectory(theories)
endif()

if(MARKETSCAN_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()

if(MARKETSCAN_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "MarketScan ${PROJECT_VERSION}")
message(STATUS "  build type ....... ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler ......... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  gui .............. ${MARKETSCAN_BUILD_GUI}")
message(STATUS "  tests ............ ${MARKETSCAN_BUILD_TESTS}")
message(STATUS "  theories ......... ${MARKETSCAN_BUILD_THEORIES}")
message(STATUS "  python ........... ${MARKETSCAN_BUILD_PYTHON}")
message(STATUS "  runtime dir ...... ${MARKETSCAN_RUNTIME_DIR}")
message(STATUS "")
