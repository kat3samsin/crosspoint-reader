#pragma once

#include <string>
#include <string_view>

#include <ReadestProgressSidecar.h>

namespace ReadestProgressStore {

enum class ManifestOwnership {
  NoManifest,
  Owned,
  Unowned,
  Unknown,
};

bool loadReadest(std::string_view document, ReadestProgress::ReadestSidecar& sidecar);
bool loadCrossPoint(std::string_view document, ReadestProgress::CrossPointSidecar& sidecar);
bool saveCrossPoint(ReadestProgress::CrossPointSidecar& sidecar);
std::string calculateRevision(std::string_view document, std::string_view xpointer, float percentage);
ManifestOwnership getManifestOwnership(std::string_view rootBookPath);

// Remove older root-level EPUB copies with the same KOReader/Readest content
// identity as a manifest-owned book. The manifest-owned path is preserved.
void removeDuplicateRootBooks(std::string_view canonicalPath);

}  // namespace ReadestProgressStore
