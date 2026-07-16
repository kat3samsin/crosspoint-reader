add_executable(ReadestThemeLayoutTest
  ReadestThemeLayoutTest.cpp
)

target_include_directories(ReadestThemeLayoutTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ReadestThemeLayoutTest PRIVATE
  crosspoint_test_common
)

add_test(NAME ReadestThemeLayoutTest COMMAND ReadestThemeLayoutTest)
