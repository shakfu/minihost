# minihost_desktop - GUI host application (stub)
#
# Status: scaffold only. See docs/dev/desktop_app.md for the full design
# and docs/dev/desktop_app_todo.md for the work breakdown.
#
# This sub-project is added by the top-level CMakeLists.txt only when
# MINIHOST_BUILD_DESKTOP is ON. It links the `minihost_gui` sibling
# static library (non-headless variant of libminihost) so plugin editor
# windows are available. The headless `minihost` archive and the
# Python wheel build are unaffected and continue to coexist in the
# same build tree.

if(NOT MINIHOST_BUILD_DESKTOP)
    return()
endif()

# Deduplicate static libraries on the link line (CMake 3.29+). Without
# this, libminihost.a appears twice and the linker emits a "duplicate
# libraries" warning. Set locally so the project's 3.20 minimum stays.
if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()

add_executable(minihost_desktop
    src/main.cpp
)

target_include_directories(minihost_desktop PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# TODO(Phase 1): minihost_audio is currently omitted because its PUBLIC
# link on `minihost` (headless) would collide with `minihost_gui` here
# (overlapping mh_* symbols). Once the desktop needs audio file or
# device I/O, split minihost_audio the same way libminihost was split
# (sibling `minihost_audio_gui` linked against `minihost_gui`).
target_link_libraries(minihost_desktop PRIVATE
    minihost_gui
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
)

target_compile_definitions(minihost_desktop PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
)

if(APPLE)
    set_target_properties(minihost_desktop PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "minihost"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.minihost.desktop"
    )
    target_link_libraries(minihost_desktop PRIVATE
        "-framework AudioUnit"
        "-framework AudioToolbox"
        "-framework CoreAudioKit"
    )
endif()
