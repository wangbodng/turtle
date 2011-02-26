project(turtle)
cmake_minimum_required(VERSION 2.8)
include(FindThreads)

set(IMPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/extern/state-threads)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/extern/include)
link_directories(${CMAKE_CURRENT_BINARY_DIR}/extern/lib)
include_directories(/usr/include/glib-2.0/ /usr/lib/glib-2.0/include/)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 -g3 -ggdb -Wall -Wextra -pedantic")

add_subdirectory(extern)

add_executable(turtle turtle.c)
add_dependencies(turtle libhaildb)
target_link_libraries(turtle ${CMAKE_THREAD_LIBS_INIT} glib-2.0 gthread-2.0 st haildb z)