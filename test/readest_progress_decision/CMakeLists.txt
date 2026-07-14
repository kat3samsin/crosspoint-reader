add_executable(ReadestProgressDecisionTest
  ReadestProgressDecisionTest.cpp
  ${REPO_ROOT}/lib/ReadestProgress/ReadestProgressDecision.cpp
)

target_link_libraries(ReadestProgressDecisionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadestProgressDecisionTest)
