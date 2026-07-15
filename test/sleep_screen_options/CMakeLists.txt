add_executable(SleepScreenOptionsTest
  SleepScreenOptionsTest.cpp
)

target_include_directories(SleepScreenOptionsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/I18n
)

target_link_libraries(SleepScreenOptionsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepScreenOptionsTest)
