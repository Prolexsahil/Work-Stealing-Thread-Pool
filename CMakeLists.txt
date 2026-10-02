cmake_minimum_required(VERSION 3.14)
project(workstealing_threadpool CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(threadpool INTERFACE)            # header-only
target_include_directories(threadpool INTERFACE include)
target_link_libraries(threadpool INTERFACE Threads::Threads)

foreach(target tests:tests/test_main.cpp
               benchmark:bench/benchmark.cpp
               basic_usage:examples/basic_usage.cpp
               deadlock_demo:examples/deadlock_demo.cpp)
  string(REPLACE ":" ";" parts ${target})
  list(GET parts 0 name)
  list(GET parts 1 src)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE threadpool)
  if(NOT MSVC)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
  endif()
endforeach()

enable_testing()
add_test(NAME tests COMMAND tests)
