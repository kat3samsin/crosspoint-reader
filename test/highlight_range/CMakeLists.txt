add_executable(highlight_range_test
  HighlightRangeTest.cpp
  ${REPO_ROOT}/lib/Highlights/HighlightRange.cpp
)

target_link_libraries(highlight_range_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(highlight_range_test)
