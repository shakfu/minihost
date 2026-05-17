# libminihost - Static library for audio plugin hosting
#
# Two sibling targets share the same sources:
#
#   - `minihost`     : headless (juce_audio_processors_headless). Default
#                      consumer for the Python wheel, CLI tools, and any
#                      build that doesn't host plugin editor windows.
#   - `minihost_gui` : non-headless (juce_audio_processors + GUI modules).
#                      Built only when MINIHOST_BUILD_DESKTOP=ON or
#                      MINIHOST_BUILD_GUI_LIB=ON. Required by the
#                      minihost_desktop app for plugin editor hosting.
#
# Both archives are produced in the same build tree; switching the
# desktop app on does not invalidate the headless library or the Python
# wheel.

option(MINIHOST_BUILD_GUI_LIB
       "Also build the minihost_gui (non-headless) static library"
       OFF)

set(_minihost_sources
    minihost.cpp
    minihost_chain.cpp
    minihost_graph.cpp
    minihost_graph_v2.cpp
    minihost_vstpreset.c
)

# Common configuration applied to both flavours. JUCE module selection
# is the only thing that differs between them.
function(_minihost_configure_common target)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_LV2=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
    )
    if(APPLE)
        target_link_libraries(${target} PRIVATE
            "-framework AudioUnit"
            "-framework AudioToolbox"
            "-framework CoreAudioKit"
        )
    endif()
endfunction()

# Headless variant -- always built.
add_library(minihost STATIC ${_minihost_sources})
_minihost_configure_common(minihost)
target_compile_definitions(minihost PRIVATE MINIHOST_HEADLESS=1)
target_link_libraries(minihost PRIVATE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_audio_processors_headless
)

# GUI variant -- opt-in. Same sources, full juce_audio_processors so
# AudioProcessorEditor and friends are available to consumers.
if(MINIHOST_BUILD_DESKTOP OR MINIHOST_BUILD_GUI_LIB)
    add_library(minihost_gui STATIC ${_minihost_sources})
    _minihost_configure_common(minihost_gui)
    target_link_libraries(minihost_gui PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_processors
    )
endif()

# Install the static library archive. No EXPORT here: see the comment in
# the top-level CMakeLists.txt MINIHOST_INSTALL block for why we don't
# generate a find_package(minihost) config target.
if(MINIHOST_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS minihost
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
