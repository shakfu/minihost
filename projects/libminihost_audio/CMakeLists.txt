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
    audio_ringbuffer.cpp
    minihost_midi.cpp
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

    # libremidi header-only mode.
    target_compile_definitions(${target} PRIVATE
        LIBREMIDI_HEADER_ONLY=1
    )

    # minihost_audio.c uses C11 <stdatomic.h> (_Atomic) for the audio
    # input-callback pointer. MSVC gates C11 atomics behind an opt-in flag
    # (VS 2022 17.5+); Clang/GCC need nothing. Scope it to C sources so the
    # C++ sources (which use std::atomic) are unaffected.
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:/experimental:c11atomics>)
    endif()

    if(APPLE)
        target_link_libraries(${target} PRIVATE
            "-framework CoreAudio"
            "-framework AudioToolbox"
            "-framework CoreFoundation"
            "-framework CoreMIDI"
        )
    elseif(WIN32)
        target_link_libraries(${target} PRIVATE winmm)
    elseif(UNIX AND NOT APPLE)
        find_package(ALSA QUIET)
        if(ALSA_FOUND)
            target_link_libraries(${target} PRIVATE ${ALSA_LIBRARIES})
            target_include_directories(${target} PRIVATE ${ALSA_INCLUDE_DIRS})
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
