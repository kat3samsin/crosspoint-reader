#pragma once

#include <cstdint>
#include <string_view>

namespace ReadestProgress {

enum class ApplyDecision : uint8_t {
  APPLY,
  DOCUMENT_MISMATCH,
  INVALID_REVISION,
  INVALID_POSITION,
  ALREADY_APPLIED,
  LOCAL_PROGRESS_CHANGED,
  BASELINE_MISMATCH,
};

struct LocalProgressPosition {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
};

struct ApplyInput {
  std::string_view expectedDocument;
  std::string_view readestDocument;
  std::string_view readestRevision;
  std::string_view basedOnCrosspoint;
  bool hasCrosspointSidecar = false;
  std::string_view crosspointDocument;
  std::string_view crosspointRevision;
  std::string_view appliedReadest;
  LocalProgressPosition localPosition;
  LocalProgressPosition sidecarPosition;
};

bool isDocumentId(std::string_view document);
bool isRevision(std::string_view revision);
bool isInitialPosition(const LocalProgressPosition& position);
bool isPersistedPosition(const LocalProgressPosition& position);
bool sameLocalPosition(const LocalProgressPosition& first, const LocalProgressPosition& second);
bool shouldPreserveAppliedRevision(std::string_view appliedReadest, const LocalProgressPosition& appliedPosition,
                                   const LocalProgressPosition& currentPosition);
bool shouldAcknowledgeAfterRender(const LocalProgressPosition& requestedPosition,
                                  const LocalProgressPosition& renderedPosition, bool renderSucceeded);
ApplyDecision decideApply(const ApplyInput& input);

}  // namespace ReadestProgress
