cmake_minimum_required(VERSION 3.16)
project(gluttonous_snake_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(snake_server
    src/main.cpp
)

target_compile_options(snake_server PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
