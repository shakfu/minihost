cmake_minimum_required(VERSION 3.20)

# Single source of truth for the release version: the `version` field in
# pyproject.toml. Wheel builds get it handed to them by scikit-build-core as
# SKBUILD_PROJECT_VERSION; standalone builds parse the same line out of the
# same file. Nothing else in the tree is allowed to spell the version out --
# the CLI binaries and the desktop app read it from the generated
# minihost_version.h below, so they cannot drift apart from the Python
# package. tests/test_release_version.py enforces this.
#
# Note this is NOT the MH_API_VERSION_* in projects/libminihost/minihost.h:
# that versions the C ABI and is bumped on its own schedule.
if(DEFINED SKBUILD_PROJECT_VERSION)
    set(MINIHOST_VERSION "${SKBUILD_PROJECT_VERSION}")
else()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml" _minihost_pyproject)
    string(REGEX MATCH "\nversion = \"([0-9]+\\.[0-9]+\\.[0-9]+[^\"]*)\""
           _minihost_version_match "\n${_minihost_pyproject}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR
            "Could not read 'version = \"X.Y.Z\"' from pyproject.toml. "
            "The release version has exactly one source of truth; fix it there.")
    endif()
    set(MINIHOST_VERSION "${CMAKE_MATCH_1}")
endif()

# Handle scikit-build-core vs standalone build
if(DEFINED SKBUILD_PROJECT_NAME)
    project(${SKBUILD_PROJECT_NAME} VERSION ${SKBUILD_PROJECT_VERSION} LANGUAGES C CXX)
else()
    project(minihost VERSION ${MINIHOST_VERSION} LANGUAGES C CXX)
endif()

# project() splits the version for us; re-derive the parts for the header.
set(MINIHOST_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(MINIHOST_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(MINIHOST_VERSION_PATCH ${PROJECT_VERSION_PATCH})

# Generate minihost_version.h and expose it as an INTERFACE target. Any target
# that wants to report its version links `minihost_version` and includes
# <minihost_version.h>; the header is regenerated whenever pyproject.toml
# changes, so a version bump needs no other edit.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/minihost_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/minihost_version.h"
    @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml")

add_library(minihost_version INTERFACE)
target_include_directories(minihost_version
    INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")

message(STATUS "minihost release version: ${MINIHOST_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Allow JUCE_PATH to be set externally, default to ./thirdparty/JUCE
if(NOT DEFINED JUCE_PATH)
    set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/JUCE")
endif()

if(NOT EXISTS "${JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${JUCE_PATH}. Run 'python scripts/download_juce.py' or set -DJUCE_PATH=/path/to/JUCE")
endif()

add_subdirectory(${JUCE_PATH} ${CMAKE_CURRENT_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)

# Install option must be set before subdirectories so install(TARGETS ... EXPORT)
# rules inside them fire when needed.
if(DEFINED SKBUILD)
    set(_MINIHOST_INSTALL_DEFAULT OFF)
else()
    set(_MINIHOST_INSTALL_DEFAULT ON)
endif()
option(MINIHOST_INSTALL
       "Generate install rules for libminihost and headers (standalone builds)"
       ${_MINIHOST_INSTALL_DEFAULT})

# Add project subdirectories
add_subdirectory(projects/libminihost)
add_subdirectory(projects/libminihost_audio)

# midifile library (MIDI file read/write) - build library only, skip tools
set(BUILD_MIDILIBRARY_ONLY ON CACHE BOOL "Build only the midifile library" FORCE)
add_subdirectory(projects/midifile EXCLUDE_FROM_ALL)

# Only build CLI tools for standalone builds (not Python wheel builds)
if(NOT DEFINED SKBUILD)
    add_subdirectory(projects/minihost_c)
    add_subdirectory(projects/minihost_cpp)
endif()

# Desktop GUI application. Opt-in via -DMINIHOST_BUILD_DESKTOP=ON. The app
# hosts plugin editor windows, so it links the non-headless `minihost_gui`
# sibling archive, which this option also builds; the headless `minihost`
# archive used by the CLI and the Python wheel is built either way and is
# unaffected. Default OFF so those builds do not pay for the JUCE GUI
# modules. See docs/dev/desktop_app.md.
option(MINIHOST_BUILD_DESKTOP "Build the minihost_desktop GUI application" OFF)
if(MINIHOST_BUILD_DESKTOP)
    add_subdirectory(projects/minihost_desktop)
endif()

if(MINIHOST_INSTALL)
    include(GNUInstallDirs)

    # Install the public C/C++ headers. The static library archive itself
    # is installed by projects/libminihost/CMakeLists.txt.
    #
    # We deliberately do NOT generate a find_package(minihost) config or
    # an exported target. The static library has PRIVATE link dependencies
    # on JUCE modules (juce_core, juce_audio_basics,
    # juce_audio_processors_headless) that this project vendors via
    # add_subdirectory of a downloaded JUCE source tree -- those targets
    # are not themselves installable and therefore cannot be exported as
    # part of minihost's interface. Downstream C/C++ consumers should
    # either rebuild minihost from source as a subdirectory, or link the
    # installed libminihost.a manually together with their own JUCE build.
    install(FILES
        projects/libminihost/minihost.h
        projects/libminihost/minihost_chain.h
        projects/libminihost/minihost_graph.h
        projects/libminihost/minihost_vstpreset.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/minihost)
endif()

# Python bindings (only when building with scikit-build-core)
if(DEFINED SKBUILD)
    find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
    find_package(nanobind CONFIG REQUIRED)

    nanobind_add_module(_core src/minihost/_core.cpp)
    # minihost is pulled in transitively via minihost_audio; listing it here
    # too causes the linker to emit 'ignoring duplicate libraries' warnings.
    # JUCE is linked explicitly so _core.cpp can include juce_audio_basics
    # for the AudioBuffer wrapper (juce::AudioBuffer<float> exposed to Python).
    target_link_libraries(_core PRIVATE minihost_audio midifile
                                        juce::juce_core juce::juce_audio_basics)
    # Linking juce_core compiles it into this target, so libminihost's flags
    # do not apply. Nothing in the bindings uses juce::URL, and leaving curl
    # on would make the wheel depend on a system libcurl.
    target_compile_definitions(_core PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
    )
    target_include_directories(_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/libminihost
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/libminihost_audio
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/miniaudio
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/midifile/include)

    install(TARGETS _core DESTINATION minihost)
endif()
