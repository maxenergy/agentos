cmake_minimum_required(VERSION 3.16)

project(submariner_clock_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(submariner_clock_server main.cpp)

if(WIN32)
    target_link_libraries(submariner_clock_server PRIVATE Ws2_32)
endif()

if(MSVC)
    target_compile_options(submariner_clock_server PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(submariner_clock_server PRIVATE -Wall -Wextra -Wpedantic)
endif()
