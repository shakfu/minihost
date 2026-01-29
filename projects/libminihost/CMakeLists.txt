# libminihost - Static library for audio plugin hosting

add_library(minihost STATIC
    minihost.cpp
)

# Required for linking into shared libraries (e.g., Python module)
set_target_properties(minihost PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
