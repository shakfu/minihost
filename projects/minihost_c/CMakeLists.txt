# minihost_c - C CLI tool for audio plugin hosting

add_executable(minihost_c main.c)
target_link_libraries(minihost_c PRIVATE minihost)
