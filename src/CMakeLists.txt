add_library(shift STATIC shift.c)

target_include_directories(shift
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(shift PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(shift PUBLIC shift_sanitizers)
