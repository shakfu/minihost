cmake_minimum_required(VERSION 3.15...3.30)
project(${SKBUILD_PROJECT_NAME} VERSION ${SKBUILD_PROJECT_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(nanobind CONFIG REQUIRED)

# Allow JUCE_PATH to be set externally, default to ../../JUCE (project root)
if(NOT DEFINED JUCE_PATH)
    set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../JUCE")
endif()

if(NOT EXISTS "${JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${JUCE_PATH}. Run ./scripts/download_juce.sh from project root or set -DJUCE_PATH=/path/to/JUCE")
endif()

add_subdirectory(${JUCE_PATH} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)

# Build the minihost static library
add_library(minihost_lib STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../../minihost.cpp)
target_include_directories(minihost_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(minihost_lib PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_audio_processors
)

if(APPLE)
    target_link_libraries(minihost_lib PRIVATE
        "-framework AudioUnit"
        "-framework AudioToolbox"
        "-framework CoreAudioKit"
    )
endif()

target_compile_definitions(minihost_lib PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

# Build the nanobind module
nanobind_add_module(_core src/minihost/_core.cpp)
target_link_libraries(_core PRIVATE minihost_lib)
target_include_directories(_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

install(TARGETS _core DESTINATION minihost)
