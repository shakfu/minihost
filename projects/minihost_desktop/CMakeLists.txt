# minihost_desktop - GUI host application
#
# A working node-graph plugin host: load VST3/AU/LV2 plugins, wire them into a
# DAG on a canvas, open plugin editor windows, render offline, and drive a
# realtime audio device (Phase 2). See docs/dev/desktop_app.md for the design
# and docs/dev/desktop_app_todo.md for the status of individual features.
# Packaging (signing / notarization / installers) is not yet wired up.
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
    src/project.cpp
    src/canvas.cpp
    src/live.cpp
    src/node_registry.cpp
    src/plugin_scanner.cpp
)

target_include_directories(minihost_desktop PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/projects/midifile/include
)

target_link_libraries(minihost_desktop PRIVATE
    minihost_gui
    minihost_audio_gui
    midifile
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

# On Windows this is a plain add_executable, i.e. the CONSOLE subsystem
# (the console CRT startup references `main`). That is deliberate: the
# headless modes (--render-project, --scan-plugins, the CI smoke step)
# need working stdout/stderr, and the out-of-process scanner relaunches
# this binary as a child worker. But START_JUCE_APPLICATION only emits an
# int main() when _CONSOLE is defined -- otherwise it emits WinMain(),
# leaving `main` unresolved (LNK2019). CMake does not define _CONSOLE for
# a console executable the way the VS project templates do, so define it
# here to keep JUCE's entry point matched to the subsystem.
if(MSVC)
    target_compile_definitions(minihost_desktop PRIVATE _CONSOLE)
endif()

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
