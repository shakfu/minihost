cmake_minimum_required(VERSION 3.20)

# Handle scikit-build-core vs standalone build
if(DEFINED SKBUILD_PROJECT_NAME)
    project(${SKBUILD_PROJECT_NAME} VERSION ${SKBUILD_PROJECT_VERSION} LANGUAGES C CXX)
else()
    project(minihost C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Allow JUCE_PATH to be set externally, default to ./JUCE
if(NOT DEFINED JUCE_PATH)
    set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
endif()

if(NOT EXISTS "${JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${JUCE_PATH}. Run ./scripts/download_juce.sh or set -DJUCE_PATH=/path/to/JUCE")
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
    target_include_directories(_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/libminihost
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/libminihost_audio
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/miniaudio
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/midifile/include)

    install(TARGETS _core DESTINATION minihost)
endif()
