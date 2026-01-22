cmake_minimum_required(VERSION 3.20)
project(minijucehost C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# Allow JUCE_PATH to be set externally, default to ./JUCE
if(NOT DEFINED JUCE_PATH)
    set(JUCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/JUCE")
endif()

if(NOT EXISTS "${JUCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${JUCE_PATH}. Run ./scripts/download_juce.sh or set -DJUCE_PATH=/path/to/JUCE")
endif()

add_subdirectory(${JUCE_PATH} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)

add_library(minihost STATIC
    minihost.cpp
)
target_include_directories(minihost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(minihost PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_audio_processors
)

if(APPLE)
    target_link_libraries(minihost PRIVATE
        "-framework AudioUnit"
        "-framework AudioToolbox"
        "-framework CoreAudioKit"
    )
endif()

# Enable plugin hosting formats
target_compile_definitions(minihost PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

# C CLI tool
add_executable(minihost_c main.c)
target_link_libraries(minihost_c PRIVATE minihost)

# C++ CLI tool (uses CLI11)
add_executable(minihost_cpp main.cpp)
target_include_directories(minihost_cpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(minihost_cpp PRIVATE minihost)

