# libminihost_audio - Audio device support using miniaudio and libremidi
#
# Mirrors the libminihost split: two sibling targets share the same
# sources but link different libminihost flavours.
#
#   - `minihost_audio`     : PUBLIC-links headless `minihost`. Default
#                            consumer for the Python wheel and CLI tools.
#   - `minihost_audio_gui` : PUBLIC-links `minihost_gui` (non-headless).
#                            Built only when MINIHOST_BUILD_DESKTOP=ON or
#                            MINIHOST_BUILD_GUI_LIB=ON. Used by the
#                            minihost_desktop app.
#
# Both archives coexist in the same build tree.

set(_minihost_audio_sources
    minihost_audio.c
    minihost_audiofile.c
    midi_ringbuffer.cpp
    param_ringbuffer.cpp
    transport_ringbuffer.cpp
    audio_ringbuffer.cpp
    minihost_midi.cpp
    minihost_osc.cpp
    ${CMAKE_SOURCE_DIR}/projects/tflac/tflac.c
)

# Common configuration applied to both flavours. The libminihost
# variant linked PUBLIC is the only thing that differs.
function(_minihost_audio_configure_common target)
    # C++20 required for libremidi.
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )

    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_include_directories(${target} PRIVATE
        ${CMAKE_SOURCE_DIR}/projects/miniaudio
        ${CMAKE_SOURCE_DIR}/projects/libminihost
        ${CMAKE_SOURCE_DIR}/projects/libremidi/include
        ${CMAKE_SOURCE_DIR}/projects/tflac
    )

    # juce_osc: OSC transport. Depends only on juce_events, which
    # juce_audio_processors_headless already pulls in, so this adds no new
    # dependency and nothing GUI. Linked PRIVATE because the C API in
    # minihost_osc.h exposes no JUCE type -- consumers never see it.
    target_link_libraries(${target} PRIVATE juce::juce_osc)

    # libremidi header-only mode.
    #
    # Header-only mode bypasses libremidi's own CMakeLists, so the platform
    # back-end macro that its cmake/libremidi.<platform>.cmake would have set
    # has to be defined here too. Without one, backends.hpp falls through to
    # `#define LIBREMIDI_DUMMY` and the whole MIDI-port subsystem compiles to
    # a stub: enumeration returns nothing and every open fails. Linking the
    # platform framework/library is necessary but NOT sufficient -- the macro
    # is what pulls the back-end in. Keep these in sync with
    # projects/libremidi/cmake/libremidi.{macos,winmm,alsa}.cmake.
    target_compile_definitions(${target} PRIVATE
        LIBREMIDI_HEADER_ONLY=1
    )

    # juce_osc pulls juce_core into this target's own compilation, so the
    # JUCE feature flags libminihost sets do not reach it. Without
    # JUCE_USE_CURL=0 juce_core compiles its curl-backed URL implementation
    # on Linux and the link fails on -lcurl, which nothing here needs.
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
    )

    if(APPLE)
        target_compile_definitions(${target} PRIVATE LIBREMIDI_COREMIDI)
        target_link_libraries(${target} PRIVATE
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework CoreFoundation"
            "-framework CoreMIDI"
        )
    elseif(WIN32)
        target_compile_definitions(${target} PRIVATE
            LIBREMIDI_WINMM
            UNICODE=1
            _UNICODE=1
        )
        target_link_libraries(${target} PRIVATE winmm)
    elseif(UNIX AND NOT APPLE)
        find_package(ALSA QUIET)
        if(ALSA_FOUND)
            target_compile_definitions(${target} PRIVATE LIBREMIDI_ALSA)
            target_link_libraries(${target} PRIVATE ${ALSA_LIBRARIES} ${CMAKE_DL_LIBS})
            target_include_directories(${target} PRIVATE ${ALSA_INCLUDE_DIRS})
        else()
            message(WARNING
                "ALSA not found: libremidi will build with its dummy back-end, "
                "so MIDI port enumeration and MidiIn will be non-functional.")
        endif()
        target_link_libraries(${target} PRIVATE pthread m)
    endif()
endfunction()

# Headless variant -- always built.
add_library(minihost_audio STATIC ${_minihost_audio_sources})
_minihost_audio_configure_common(minihost_audio)
# PUBLIC: the archive references mh_* symbols defined in libminihost;
# consumers need libminihost on their final link line and (typically)
# need minihost.h on their include path too. Forwarding via PUBLIC
# avoids each consumer having to list 'minihost' explicitly (which
# would duplicate it on the link line).
target_link_libraries(minihost_audio PUBLIC minihost)

# GUI variant -- opt-in. Same sources, PUBLIC-links minihost_gui.
if(MINIHOST_BUILD_DESKTOP OR MINIHOST_BUILD_GUI_LIB)
    add_library(minihost_audio_gui STATIC ${_minihost_audio_sources})
    _minihost_audio_configure_common(minihost_audio_gui)
    target_link_libraries(minihost_audio_gui PUBLIC minihost_gui)
endif()
