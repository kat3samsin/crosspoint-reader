#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Highlights/HighlightRange.h"

TEST(HighlightRange, RoundTripsStableWordRanges) {
  const std::vector<Highlights::Range> expected = {{0, 4, 9}, {2, 100, 140}};
  std::string data;
  ASSERT_TRUE(Highlights::serializeRanges(expected, data));
  EXPECT_EQ(data, "crosspoint-highlights-v1\n0 4 9\n2 100 140\n");

  std::vector<Highlights::Range> parsed;
  ASSERT_TRUE(Highlights::parseRanges(data, parsed));
  ASSERT_EQ(parsed.size(), expected.size());
  EXPECT_EQ(parsed[0].spineIndex, 0);
  EXPECT_EQ(parsed[0].startWord, 4u);
  EXPECT_EQ(parsed[0].endWord, 9u);
  EXPECT_EQ(parsed[1].spineIndex, 2);
  EXPECT_EQ(parsed[1].startWord, 100u);
  EXPECT_EQ(parsed[1].endWord, 140u);
}

TEST(HighlightRange, MatchesOnlyItsSpineAndInclusiveEndpoints) {
  const Highlights::Range range{3, 20, 22};
  EXPECT_FALSE(Highlights::contains(range, 2, 21));
  EXPECT_TRUE(Highlights::contains(range, 3, 20));
  EXPECT_TRUE(Highlights::contains(range, 3, 21));
  EXPECT_TRUE(Highlights::contains(range, 3, 22));
  EXPECT_FALSE(Highlights::contains(range, 3, 23));
}

TEST(HighlightRange, RejectsMalformedOrUnsafeFiles) {
  std::vector<Highlights::Range> ranges;
  EXPECT_FALSE(Highlights::parseRanges("wrong\n0 1 2\n", ranges));
  EXPECT_FALSE(Highlights::parseRanges("crosspoint-highlights-v1\n0 9 2\n", ranges));
  EXPECT_FALSE(Highlights::parseRanges("crosspoint-highlights-v1\n0 1 4294967295\n", ranges));
  EXPECT_FALSE(Highlights::parseRanges("crosspoint-highlights-v1\n0 1 2", ranges));
}

TEST(HighlightRange, TreatsAMissingFileAsNoHighlights) {
  std::vector<Highlights::Range> ranges = {{1, 2, 3}};
  EXPECT_TRUE(Highlights::parseRanges("", ranges));
  EXPECT_TRUE(ranges.empty());
}
