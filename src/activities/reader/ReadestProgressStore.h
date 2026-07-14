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

}  // namespace ReadestProgressStore
