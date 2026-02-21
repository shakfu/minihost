# libminihost_audio - Audio device support using miniaudio and libremidi

add_library(minihost_audio STATIC
    minihost_audio.c
    minihost_audiofile.c
    midi_ringbuffer.cpp
    minihost_midi.cpp
    ${CMAKE_SOURCE_DIR}/projects/tflac/tflac.c
)

# Required for linking into shared libraries (e.g., Python module)
# C++20 required for libremidi
set_target_properties(minihost_audio PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(minihost_audio PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(minihost_audio PRIVATE
    ${CMAKE_SOURCE_DIR}/projects/miniaudio
    ${CMAKE_SOURCE_DIR}/projects/libminihost
    ${CMAKE_SOURCE_DIR}/projects/libremidi/include
    ${CMAKE_SOURCE_DIR}/projects/tflac
)

target_link_libraries(minihost_audio PRIVATE minihost)

# libremidi configuration - header-only mode
target_compile_definitions(minihost_audio PRIVATE
    LIBREMIDI_HEADER_ONLY=1
)

# Platform audio and MIDI frameworks
if(APPLE)
    target_link_libraries(minihost_audio PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreMIDI"
    )
elseif(WIN32)
    target_link_libraries(minihost_audio PRIVATE winmm)
elseif(UNIX AND NOT APPLE)
    # Linux - link pthread, math, and ALSA
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        target_link_libraries(minihost_audio PRIVATE ${ALSA_LIBRARIES})
        target_include_directories(minihost_audio PRIVATE ${ALSA_INCLUDE_DIRS})
    endif()
    target_link_libraries(minihost_audio PRIVATE pthread m)
endif()
