#include "ReadestProgressDecision.h"

namespace ReadestProgress {
namespace {

bool isLowercaseMd5(const std::string_view value) {
  if (value.size() != 32) {
    return false;
  }
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool isOptionalRevision(const std::string_view revision) { return revision.empty() || isRevision(revision); }

}  // namespace

bool isDocumentId(const std::string_view document) { return isLowercaseMd5(document); }

bool isRevision(const std::string_view revision) { return isLowercaseMd5(revision); }

bool isInitialPosition(const LocalProgressPosition& position) {
  return position.spineIndex == 0 && position.pageNumber == 0 && position.pageCount == 0;
}

bool isPersistedPosition(const LocalProgressPosition& position) {
  return position.spineIndex >= 0 && position.pageNumber >= 0 && position.pageCount > 0 &&
         position.pageNumber < position.pageCount;
}

bool sameLocalPosition(const LocalProgressPosition& first, const LocalProgressPosition& second) {
  return first.spineIndex == second.spineIndex && first.pageNumber == second.pageNumber &&
         first.pageCount == second.pageCount;
}

bool shouldPreserveAppliedRevision(const std::string_view appliedReadest,
                                   const LocalProgressPosition& appliedPosition,
                                   const LocalProgressPosition& currentPosition) {
  return isRevision(appliedReadest) && isPersistedPosition(appliedPosition) &&
         isPersistedPosition(currentPosition) && sameLocalPosition(appliedPosition, currentPosition);
}

bool shouldAcknowledgeAfterRender(const LocalProgressPosition& requestedPosition,
                                  const LocalProgressPosition& renderedPosition,
                                  const bool renderSucceeded) {
  return renderSucceeded && isPersistedPosition(requestedPosition) && isPersistedPosition(renderedPosition) &&
         requestedPosition.spineIndex == renderedPosition.spineIndex &&
         requestedPosition.pageNumber == renderedPosition.pageNumber;
}

ApplyDecision decideApply(const ApplyInput& input) {
  if (!isDocumentId(input.expectedDocument) || input.readestDocument != input.expectedDocument ||
      (input.hasCrosspointSidecar && input.crosspointDocument != input.expectedDocument)) {
    return ApplyDecision::DOCUMENT_MISMATCH;
  }
  if (!isRevision(input.readestRevision) ||
      (input.hasCrosspointSidecar && !isRevision(input.crosspointRevision)) ||
      !isOptionalRevision(input.basedOnCrosspoint) || !isOptionalRevision(input.appliedReadest)) {
    return ApplyDecision::INVALID_REVISION;
  }

  if (input.hasCrosspointSidecar) {
    if (!isPersistedPosition(input.localPosition) || !isPersistedPosition(input.sidecarPosition)) {
      return ApplyDecision::INVALID_POSITION;
    }
    if (!input.appliedReadest.empty() && input.appliedReadest == input.readestRevision) {
      return ApplyDecision::ALREADY_APPLIED;
    }
    if (!sameLocalPosition(input.localPosition, input.sidecarPosition)) {
      return ApplyDecision::LOCAL_PROGRESS_CHANGED;
    }
    if (input.basedOnCrosspoint.empty() || input.basedOnCrosspoint != input.crosspointRevision) {
      return ApplyDecision::BASELINE_MISMATCH;
    }
    return ApplyDecision::APPLY;
  }

  if (!isInitialPosition(input.localPosition)) {
    return isPersistedPosition(input.localPosition) ? ApplyDecision::LOCAL_PROGRESS_CHANGED
                                                    : ApplyDecision::INVALID_POSITION;
  }
  if (!input.basedOnCrosspoint.empty()) {
    return ApplyDecision::BASELINE_MISMATCH;
  }
  return ApplyDecision::APPLY;
}

}  // namespace ReadestProgress
