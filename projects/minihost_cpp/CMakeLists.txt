# minihost_cpp - C++ CLI tool for audio plugin hosting (uses CLI11)

add_executable(minihost_cpp main.cpp)
target_include_directories(minihost_cpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../libminihost_audio
    ${CMAKE_CURRENT_SOURCE_DIR}/../midifile/include)
# minihost is pulled in transitively via minihost_audio; listing it here too
# causes the linker to emit 'ignoring duplicate libraries' warnings.
target_link_libraries(minihost_cpp PRIVATE minihost_audio midifile)
