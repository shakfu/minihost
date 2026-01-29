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

add_subdirectory(${JUCE_PATH} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)

# Add project subdirectories
add_subdirectory(projects/libminihost)
add_subdirectory(projects/libminihost_audio)

# Only build CLI tools for standalone builds (not Python wheel builds)
if(NOT DEFINED SKBUILD)
    add_subdirectory(projects/minihost_c)
    add_subdirectory(projects/minihost_cpp)
endif()

# Python bindings (only when building with scikit-build-core)
if(DEFINED SKBUILD)
    find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
    find_package(nanobind CONFIG REQUIRED)

    nanobind_add_module(_core src/minihost/_core.cpp)
    target_link_libraries(_core PRIVATE minihost minihost_audio)
    target_include_directories(_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/libminihost
        ${CMAKE_CURRENT_SOURCE_DIR}/projects/libminihost_audio)

    install(TARGETS _core DESTINATION minihost)
endif()
