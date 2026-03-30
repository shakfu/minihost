# minihost_c - C CLI tool for audio plugin hosting

add_executable(minihost_c main.c)
target_include_directories(minihost_c PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../libminihost_audio)
target_link_libraries(minihost_c PRIVATE minihost minihost_audio)
