cmake_minimum_required(VERSION 3.16)
project(cpp_http_server VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(httpd
    src/main.cpp
    src/server.cpp
    src/http.cpp
    src/mime.cpp
)

target_include_directories(httpd PRIVATE src)

# Strict warnings; the build must stay clean under these.
target_compile_options(httpd PRIVATE -Wall -Wextra -Werror -O2)

# POSIX threads for the worker pool.
target_link_libraries(httpd PRIVATE pthread)
