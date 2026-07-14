add_executable(ProgressXPathParserTest
  ProgressXPathParserTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ProgressXPathParser.cpp
)

target_link_libraries(ProgressXPathParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressXPathParserTest)
