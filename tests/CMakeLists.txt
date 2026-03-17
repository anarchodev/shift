include(FetchContent)

FetchContent_Declare(
  unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG        v2.6.0
)
FetchContent_MakeAvailable(unity)

add_executable(shift_tests test_shift.c)

target_link_libraries(shift_tests PRIVATE shift unity shift_sanitizers)

add_test(NAME shift_tests COMMAND shift_tests)
