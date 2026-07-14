#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "ReadestProgressDecision.h"

namespace ReadestProgress {

inline constexpr int SIDECAR_SCHEMA_VERSION = 2;
inline constexpr size_t MAX_SIDECAR_BYTES = 2048;
inline constexpr size_t MAX_XPOINTER_BYTES = 1024;
inline constexpr std::string_view SIDECAR_DIRECTORY = "/.crosspoint/readest-sync";

struct ReadestSidecar {
  std::string document;
  std::string revision;
  std::string xpointer;
  float percentage = 0.0f;
  std::string basedOnCrosspoint;
};

struct CrossPointSidecar {
  std::string document;
  std::string revision;
  std::string xpointer;
  float percentage = 0.0f;
  std::string appliedReadest;
  LocalProgressPosition position;
};

bool parseReadestSidecar(std::string_view json, ReadestSidecar& sidecar);
bool parseCrossPointSidecar(std::string_view json, CrossPointSidecar& sidecar);
bool serializeReadestSidecar(const ReadestSidecar& sidecar, std::string& json);
bool serializeCrossPointSidecar(const CrossPointSidecar& sidecar, std::string& json);

bool buildCrossPointRevisionInput(std::string_view document, std::string_view xpointer, float percentage,
                                  std::string& input);
bool matchesCrossPointRevision(const CrossPointSidecar& sidecar, std::string_view calculatedRevision);
CrossPointSidecar createCrossPointSidecar(std::string_view document, std::string_view revision,
                                          std::string_view xpointer, float percentage,
                                          std::string_view appliedReadest, const LocalProgressPosition& position);
std::string appliedRevisionForExit(const CrossPointSidecar* previous, const LocalProgressPosition& currentPosition);

std::string readestSidecarPath(std::string_view document);
std::string crossPointSidecarPath(std::string_view document);

}  // namespace ReadestProgress
