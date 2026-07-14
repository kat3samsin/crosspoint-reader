#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "KOReaderSync/ProgressXPathParser.h"

namespace {

ProgressXPath::ParsedXPath parseValid(const std::string_view xpath) {
  ProgressXPath::ParsedXPath parsed{};
  EXPECT_TRUE(ProgressXPath::parse(xpath, parsed)) << xpath;
  return parsed;
}

TEST(ProgressXPathParser, ParsesChapterRoots) {
  for (const std::string_view xpath : {"/body/DocFragment[22]", "/body/DocFragment[22].0",
                                       "/body/DocFragment[22]/body", "/body/DocFragment[22]/body/"}) {
    const auto parsed = parseValid(xpath);
    EXPECT_EQ(parsed.documentFragment, 22);
    EXPECT_EQ(parsed.stepCount, 0);
  }
}

TEST(ProgressXPathParser, ParsesElementOnlyAncestryAndImplicitIndexes) {
  const auto parsed = parseValid("/body/DocFragment[22]/body/div/p[128]");

  ASSERT_EQ(parsed.stepCount, 2);
  EXPECT_STREQ(parsed.steps[0].tag, "div");
  EXPECT_EQ(parsed.steps[0].siblingIndex, 1);
  EXPECT_STREQ(parsed.steps[1].tag, "p");
  EXPECT_EQ(parsed.steps[1].siblingIndex, 128);
  EXPECT_FALSE(parsed.hasTextTerminal);
  EXPECT_FALSE(parsed.hasCharacterOffset);
}

TEST(ProgressXPathParser, ParsesUnindexedTextNodeOffset) {
  const auto parsed = parseValid("/body/DocFragment[22]/body/div/p[148]/text().130");

  ASSERT_EQ(parsed.stepCount, 2);
  EXPECT_TRUE(parsed.hasTextTerminal);
  EXPECT_FALSE(parsed.hasExplicitTextNodeIndex);
  EXPECT_TRUE(parsed.hasCharacterOffset);
  EXPECT_EQ(parsed.textNodeIndex, 1);
  EXPECT_EQ(parsed.characterOffset, 130u);
}

TEST(ProgressXPathParser, ParsesIndexedTextNodeOffset) {
  const auto parsed = parseValid("/body/DocFragment[22]/body/div/p[148]/text()[2].130");

  ASSERT_EQ(parsed.stepCount, 2);
  EXPECT_TRUE(parsed.hasTextTerminal);
  EXPECT_TRUE(parsed.hasExplicitTextNodeIndex);
  EXPECT_TRUE(parsed.hasCharacterOffset);
  EXPECT_EQ(parsed.textNodeIndex, 2);
  EXPECT_EQ(parsed.characterOffset, 130u);
}

TEST(ProgressXPathParser, ParsesElementOffset) {
  const auto parsed = parseValid("/body/DocFragment[22]/body/div/p[128].0");

  ASSERT_EQ(parsed.stepCount, 2);
  EXPECT_FALSE(parsed.hasTextTerminal);
  EXPECT_TRUE(parsed.hasCharacterOffset);
  EXPECT_EQ(parsed.characterOffset, 0u);
}

TEST(ProgressXPathParser, NormalizesReadestZeroElementIndexToFirstSibling) {
  const auto parsed = parseValid("/body/DocFragment[2]/body/div[0]/p[1]/text().6");

  ASSERT_EQ(parsed.stepCount, 2);
  EXPECT_EQ(parsed.steps[0].siblingIndex, 1);
  EXPECT_EQ(parsed.steps[1].siblingIndex, 1);
}

TEST(ProgressXPathParser, AcceptsSupportedNameCharactersAndNumericLimits) {
  const auto parsed =
      parseValid("/body/DocFragment[65535]/body/_custom/epub:switch[65535]/h1/text()[65535].2147483647");

  ASSERT_EQ(parsed.stepCount, 3);
  EXPECT_STREQ(parsed.steps[0].tag, "_custom");
  EXPECT_STREQ(parsed.steps[1].tag, "epub:switch");
  EXPECT_STREQ(parsed.steps[2].tag, "h1");
  EXPECT_EQ(parsed.steps[1].siblingIndex, 65535);
  EXPECT_EQ(parsed.textNodeIndex, 65535);
  EXPECT_EQ(parsed.characterOffset, 2147483647u);
}

TEST(ProgressXPathParser, AcceptsExactlyTheMaximumAncestryDepth) {
  const auto parsed = parseValid("/body/DocFragment[1]/body/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p");
  EXPECT_EQ(parsed.stepCount, ProgressXPath::MAX_DEPTH);
}

class InvalidXPathTest : public testing::TestWithParam<const char*> {};

TEST_P(InvalidXPathTest, RejectsMalformedOrOutOfRangeInput) {
  ProgressXPath::ParsedXPath parsed{};
  EXPECT_FALSE(ProgressXPath::parse(GetParam(), parsed)) << GetParam();
}

INSTANTIATE_TEST_SUITE_P(
    InvalidInputs, InvalidXPathTest,
    testing::Values("", "prefix/body/DocFragment[1]/body/p", "/body/DocFragment[]/body/p",
                    "/body/DocFragment[0]/body/p", "/body/DocFragment[65536]/body/p",
                    "/body/DocFragment[999999999999999999999999999999]/body/p",
                    "/body/DocFragment[1]junk/body/p", "/body/DocFragment[1]/other/p",
                    "/body/DocFragment[1]/body//p", "/body/DocFragment[1]/body/p[]",
                    "/body/DocFragment[1]/body/p[65536]",
                    "/body/DocFragment[1]/body/p[999999999999999999999999999999]",
                    "/body/DocFragment[1]/body/p[2]junk", "/body/DocFragment[1]/body/p[1][2]",
                    "/body/DocFragment[1]/body/p.", "/body/DocFragment[1]/body/p.not-a-number",
                    "/body/DocFragment[1]/body/p.2147483648", "/body/DocFragment[1]/body/p/text()",
                    "/body/DocFragment[1]/body/p/text()[].1", "/body/DocFragment[1]/body/p/text()[0].1",
                    "/body/DocFragment[1]/body/p/text()[65536].1",
                    "/body/DocFragment[1]/body/p/text()[1].not-a-number",
                    "/body/DocFragment[1]/body/p/text()[1].1junk", "/body/DocFragment[1]/body/p/",
                    "/body/DocFragment[1]/body/p!", "/body/DocFragment[1]/body/abcdefghijkl",
                    "/body/DocFragment[1]/body/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q"));

TEST(ProgressXPathParser, RejectsInputBeyondTheSidecarContract) {
  std::string xpath = "/body/DocFragment[1]/body/p.";
  xpath.append(ProgressXPath::MAX_XPATH_BYTES, '0');

  ProgressXPath::ParsedXPath parsed{};
  EXPECT_FALSE(ProgressXPath::parse(xpath, parsed));
}

}  // namespace
