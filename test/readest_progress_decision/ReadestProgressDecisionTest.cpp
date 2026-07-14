#include <gtest/gtest.h>

#include <string_view>

#include "ReadestProgress/ReadestProgressDecision.h"

namespace {

constexpr char DOCUMENT[] = "0123456789abcdef0123456789abcdef";
constexpr char OTHER_DOCUMENT[] = "fedcba9876543210fedcba9876543210";
constexpr char READEST_REVISION[] = "11111111111111111111111111111111";
constexpr char CROSSPOINT_REVISION[] = "22222222222222222222222222222222";

ReadestProgress::ApplyInput establishedInput() {
  return {
      .expectedDocument = DOCUMENT,
      .readestDocument = DOCUMENT,
      .readestRevision = READEST_REVISION,
      .basedOnCrosspoint = CROSSPOINT_REVISION,
      .hasCrosspointSidecar = true,
      .crosspointDocument = DOCUMENT,
      .crosspointRevision = CROSSPOINT_REVISION,
      .appliedReadest = "00000000000000000000000000000000",
      .localPosition = {4, 8, 12},
      .sidecarPosition = {4, 8, 12},
  };
}

TEST(ReadestProgressValidation, AcceptsOnlyLowercaseMd5ShapedIdentifiersAndRevisions) {
  EXPECT_TRUE(ReadestProgress::isDocumentId(DOCUMENT));
  EXPECT_TRUE(ReadestProgress::isRevision(READEST_REVISION));
  for (const std::string_view invalid : {"", "1111111111111111111111111111111",
                                         "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                                         "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"}) {
    EXPECT_FALSE(ReadestProgress::isDocumentId(invalid));
    EXPECT_FALSE(ReadestProgress::isRevision(invalid));
  }
}

TEST(ReadestProgressValidation, DistinguishesInitialAndPersistedPositions) {
  EXPECT_TRUE(ReadestProgress::isInitialPosition({0, 0, 0}));
  EXPECT_FALSE(ReadestProgress::isPersistedPosition({0, 0, 0}));
  EXPECT_TRUE(ReadestProgress::isPersistedPosition({0, 0, 1}));
  EXPECT_TRUE(ReadestProgress::isPersistedPosition({4, 8, 12}));
  EXPECT_FALSE(ReadestProgress::isInitialPosition({0, 0, 1}));
}

TEST(ReadestProgressDecision, AppliesAnUnseenReadestUpdateAgainstUnchangedLocalProgress) {
  EXPECT_EQ(ReadestProgress::decideApply(establishedInput()), ReadestProgress::ApplyDecision::APPLY);
}

TEST(ReadestProgressDecision, AppliesInitialReadestProgressOnlyAtTheExactInitialPosition) {
  auto input = establishedInput();
  input.basedOnCrosspoint = "";
  input.hasCrosspointSidecar = false;
  input.crosspointDocument = "";
  input.crosspointRevision = "";
  input.appliedReadest = "";
  input.localPosition = {0, 0, 0};
  input.sidecarPosition = {};

  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::APPLY);
}

TEST(ReadestProgressDecision, ProtectsValidExistingLocalProgressWithoutABaseline) {
  auto input = establishedInput();
  input.basedOnCrosspoint = "";
  input.hasCrosspointSidecar = false;
  input.crosspointDocument = "";
  input.crosspointRevision = "";
  input.appliedReadest = "";
  input.localPosition = {0, 1, 10};
  input.sidecarPosition = {};

  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::LOCAL_PROGRESS_CHANGED);
}

TEST(ReadestProgressDecision, RejectsMismatchedDocumentsAndRevisions) {
  auto input = establishedInput();
  input.readestDocument = OTHER_DOCUMENT;
  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::DOCUMENT_MISMATCH);

  input = establishedInput();
  input.readestRevision = "not-a-revision";
  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::INVALID_REVISION);

  input = establishedInput();
  input.crosspointRevision = "not-a-revision";
  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::INVALID_REVISION);
}

TEST(ReadestProgressDecision, SkipsAlreadyAppliedReadestProgress) {
  auto input = establishedInput();
  input.appliedReadest = READEST_REVISION;
  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::ALREADY_APPLIED);
}

TEST(ReadestProgressDecision, DetectsLocalProgressAndBaselineChanges) {
  auto input = establishedInput();
  input.localPosition.pageNumber++;
  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::LOCAL_PROGRESS_CHANGED);

  input = establishedInput();
  input.basedOnCrosspoint = "33333333333333333333333333333333";
  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::BASELINE_MISMATCH);
}

class InvalidPersistedPositionTest : public testing::TestWithParam<ReadestProgress::LocalProgressPosition> {};

TEST_P(InvalidPersistedPositionTest, NeverAppliesAnInvalidLocalTuple) {
  auto input = establishedInput();
  input.localPosition = GetParam();
  input.sidecarPosition = GetParam();
  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::INVALID_POSITION);
}

INSTANTIATE_TEST_SUITE_P(InvalidPositions, InvalidPersistedPositionTest,
                         testing::Values(ReadestProgress::LocalProgressPosition{-1, 0, 1},
                                         ReadestProgress::LocalProgressPosition{0, -1, 1},
                                         ReadestProgress::LocalProgressPosition{0, 0, -1},
                                         ReadestProgress::LocalProgressPosition{0, 0, 0},
                                         ReadestProgress::LocalProgressPosition{0, 1, 1},
                                         ReadestProgress::LocalProgressPosition{0, 2, 1},
                                         ReadestProgress::LocalProgressPosition{0, 99, 0}));

TEST(ReadestProgressDecision, RejectsAnInvalidSidecarTupleEvenWhenLocalProgressIsValid) {
  auto input = establishedInput();
  input.sidecarPosition = {0, 12, 12};
  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::INVALID_POSITION);
}

TEST(ReadestProgressDecision, RejectsMalformedInitialPositionWithoutABaseline) {
  auto input = establishedInput();
  input.basedOnCrosspoint = "";
  input.hasCrosspointSidecar = false;
  input.crosspointDocument = "";
  input.crosspointRevision = "";
  input.appliedReadest = "";
  input.localPosition = {0, 1, 0};
  input.sidecarPosition = {};

  EXPECT_EQ(ReadestProgress::decideApply(input), ReadestProgress::ApplyDecision::INVALID_POSITION);
}

TEST(ReadestProgressAcknowledgement, PreservesOnlyAValidRevisionAtTheSamePersistedPosition) {
  EXPECT_TRUE(ReadestProgress::shouldPreserveAppliedRevision(READEST_REVISION, {4, 8, 12}, {4, 8, 12}));
  EXPECT_FALSE(ReadestProgress::shouldPreserveAppliedRevision(READEST_REVISION, {4, 8, 12}, {4, 9, 12}));
  EXPECT_FALSE(ReadestProgress::shouldPreserveAppliedRevision(READEST_REVISION, {0, 0, 0}, {0, 0, 0}));
  EXPECT_FALSE(ReadestProgress::shouldPreserveAppliedRevision("invalid", {4, 8, 12}, {4, 8, 12}));
}

TEST(ReadestProgressAcknowledgement, WaitsForTheRequestedPageToRenderSuccessfully) {
  EXPECT_TRUE(ReadestProgress::shouldAcknowledgeAfterRender({4, 8, 12}, {4, 8, 12}, true));
  EXPECT_TRUE(ReadestProgress::shouldAcknowledgeAfterRender({4, 8, 12}, {4, 8, 14}, true));
  EXPECT_FALSE(ReadestProgress::shouldAcknowledgeAfterRender({4, 8, 12}, {4, 8, 12}, false));
  EXPECT_FALSE(ReadestProgress::shouldAcknowledgeAfterRender({4, 8, 12}, {4, 9, 12}, true));
  EXPECT_FALSE(ReadestProgress::shouldAcknowledgeAfterRender({4, 8, 12}, {5, 8, 12}, true));
}

}  // namespace
