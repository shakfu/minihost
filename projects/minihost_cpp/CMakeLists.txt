# minihost_cpp - C++ CLI tool for audio plugin hosting (uses CLI11)

add_executable(minihost_cpp main.cpp)
target_include_directories(minihost_cpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(minihost_cpp PRIVATE minihost)
