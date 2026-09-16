# ---------------------------------------------------------------------------
# The Python module.
#
# Binds run_backtest and the metrics -- the same code the command line calls,
# so a notebook and the desktop application cannot disagree about what
# happened.
#
# Deliberately does NOT allow writing a strategy in Python. The engine's
# contract is that a strategy is a pure function of the events it was shown;
# hanging the GIL off the per-event path would make replay speed depend on the
# interpreter, and a Python strategy closing over anything outside the engine
# would make a run irreproducible. Strategies are DLLs.
# ---------------------------------------------------------------------------

# --- Which Python -----------------------------------------------------------
#
# vcpkg's python3 port supplies the headers and import library that pybind11
# links against, and it pins them to one minor version. CMake's FindPython
# refuses to pair those with an interpreter of a different minor version, so
# the module is built for whatever version vcpkg provides -- currently 3.12 --
# and is importable from any CPython of that minor version, not just the
# interpreter that happened to be on PATH during the build.
#
# That is a property of this dependency set rather than a decision, and it is
# documented rather than worked around, because the alternative is quietly
# producing a module that imports on the build machine and nowhere else.
#
# Two traps this avoids:
#
#   * MSYS2, Cygwin and Git-for-Windows each ship a python3.exe that lands on
#     PATH and that MSVC cannot link against. pybind11's legacy
#     FindPythonLibsNew picks whichever it sees first and fails several layers
#     down with a bare "Python libraries not found".
#
#   * PYBIND11_FINDPYTHON makes pybind11 reuse what is found here rather than
#     searching again with that legacy module.
#
# Override with -DMARKETSCAN_PYTHON_EXECUTABLE=<path to a matching python.exe>
# to build against a specific interpreter of the same minor version.
set(PYBIND11_FINDPYTHON ON)
set(Python_FIND_REGISTRY FIRST)

# Ask for the minor version vcpkg's headers are, rather than the newest
# interpreter on the machine. FindPython otherwise finds 3.13, pairs it with
# vcpkg's 3.12 library, notices they disagree and reports the unhelpful
# "missing: Development.Module".
set(MARKETSCAN_PYTHON_VERSION "" CACHE STRING
    "Python minor version to target, e.g. 3.12. Empty detects it from vcpkg.")
if(NOT MARKETSCAN_PYTHON_VERSION AND DEFINED VCPKG_INSTALLED_DIR)
    file(GLOB _ms_python_includes
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include/python3.*")
    foreach(_ms_include IN LISTS _ms_python_includes)
        if(_ms_include MATCHES "python(3\.[0-9]+)$")
            set(MARKETSCAN_PYTHON_VERSION "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()

set(MARKETSCAN_PYTHON_EXECUTABLE "" CACHE FILEPATH
    "Python interpreter to build the module against. Must match "
    "MARKETSCAN_PYTHON_VERSION. Leave empty to detect.")
if(MARKETSCAN_PYTHON_EXECUTABLE)
    set(Python_EXECUTABLE "${MARKETSCAN_PYTHON_EXECUTABLE}")
endif()

if(MARKETSCAN_PYTHON_VERSION)
    # A range rather than EXACT: any patch release of that minor version is
    # ABI compatible, and demanding 3.12.0 precisely would find nothing.
    string(REPLACE "." ";" _ms_version_parts "${MARKETSCAN_PYTHON_VERSION}")
    list(GET _ms_version_parts 1 _ms_minor)
    math(EXPR _ms_next_minor "${_ms_minor} + 1")
    set(Python_FIND_STRATEGY VERSION)
    find_package(Python "${MARKETSCAN_PYTHON_VERSION}...<3.${_ms_next_minor}"
        COMPONENTS Interpreter Development.Module REQUIRED)
else()
    find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)
endif()
find_package(pybind11 CONFIG REQUIRED)

message(STATUS "MarketScan: Python module for ${Python_VERSION} (${Python_EXECUTABLE})")

# ---------------------------------------------------------------------------
pybind11_add_module(marketscan_python MODULE module.cpp)

target_link_libraries(marketscan_python PRIVATE marketscan::core marketscan::flags)

set_target_properties(marketscan_python PROPERTIES
    OUTPUT_NAME "marketscan"
    LIBRARY_OUTPUT_DIRECTORY "${MARKETSCAN_RUNTIME_DIR}"
    FOLDER "marketscan")

foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(marketscan_python PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${MARKETSCAN_RUNTIME_DIR}")
endforeach()

# Where the module landed, for the notebook and for anyone setting PYTHONPATH.
message(STATUS "MarketScan: import it with  sys.path.insert(0, '${MARKETSCAN_RUNTIME_DIR}')")

# ---------------------------------------------------------------------------
# The module is covered by ctest like everything else: it imports, runs a
# backtest over the committed sample, and checks the ledger invariant held.
# Bindings that compile but cannot be imported are a common and quiet failure.
# ---------------------------------------------------------------------------
if(MARKETSCAN_BUILD_TESTS)
    add_test(NAME python_bindings
        COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/examples/smoke_test.py"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
    set_tests_properties(python_bindings PROPERTIES
        LABELS "python"
        TIMEOUT 300)
endif()
