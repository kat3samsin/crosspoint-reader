add_executable(ReadestProgressSidecarTest
  ReadestProgressSidecarTest.cpp
  ${REPO_ROOT}/lib/ReadestProgress/ReadestProgressDecision.cpp
  ${REPO_ROOT}/lib/ReadestProgress/ReadestProgressSidecar.cpp
  ${REPO_ROOT}/lib/ReadestProgress/ReadestLibraryOwnership.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ProgressXPathParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_link_libraries(ReadestProgressSidecarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(ReadestProgressSidecarTest PRIVATE
  ${REPO_ROOT}/.pio/libdeps/katre_fast/ArduinoJson/src
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/JsonParser
)

gtest_discover_tests(ReadestProgressSidecarTest)
