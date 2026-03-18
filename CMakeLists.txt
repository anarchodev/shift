cmake_minimum_required(VERSION 3.21)
project(shift VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(SHIFT_BUILD_TESTS "Build tests" ON)

# Export compile_commands.json for debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# Sanitizer interface target
add_library(shift_sanitizers INTERFACE)
target_compile_options(shift_sanitizers INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>
)
target_link_options(shift_sanitizers INTERFACE
  $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

add_subdirectory(src)

if(SHIFT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
