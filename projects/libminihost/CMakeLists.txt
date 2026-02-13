# libminihost - Static library for audio plugin hosting

option(MINIHOST_HEADLESS "Build without GUI dependencies (default ON)" ON)

add_library(minihost STATIC
    minihost.cpp
    minihost_chain.cpp
)

# Required for linking into shared libraries (e.g., Python module)
set_target_properties(minihost PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(minihost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MINIHOST_HEADLESS)
    target_link_libraries(minihost PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_processors_headless
    )
    target_compile_definitions(minihost PRIVATE MINIHOST_HEADLESS=1)
else()
    target_link_libraries(minihost PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_processors
    )
endif()

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
    JUCE_PLUGINHOST_LV2=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)
