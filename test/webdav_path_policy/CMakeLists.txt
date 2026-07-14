add_executable(WebDAVPathPolicyTest
  WebDAVPathPolicyTest.cpp
  ${REPO_ROOT}/src/network/WebDAVPathPolicy.cpp
)

target_link_libraries(WebDAVPathPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WebDAVPathPolicyTest
  TEST_PREFIX "WebDAVPathPolicy."
)
