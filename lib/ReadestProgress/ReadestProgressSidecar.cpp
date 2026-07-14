#include "ReadestProgressSidecar.h"

#include <ArduinoJson.h>
#include <ProgressXPathParser.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ReadestProgress {
namespace {

bool isValidPercentage(const float percentage) {
  return std::isfinite(percentage) && percentage >= 0.0f && percentage <= 1.0f;
}

bool isValidXPointer(const std::string_view xpointer) {
  if (xpointer.empty() || xpointer.size() > MAX_XPOINTER_BYTES) return false;
  ProgressXPath::ParsedXPath parsed{};
  return ProgressXPath::parse(xpointer, parsed);
}

bool isBoundedPosition(const LocalProgressPosition& position) {
  constexpr int maximum = 0xffff;
  return isPersistedPosition(position) && position.spineIndex <= maximum && position.pageNumber <= maximum &&
         position.pageCount <= maximum;
}

bool hasKey(const JsonObjectConst object, const std::string_view expected) {
  for (const JsonPairConst pair : object) {
    const char* key = pair.key().c_str();
    if (key && expected == key) return true;
  }
  return false;
}

bool validateReadest(const ReadestSidecar& sidecar) {
  return isDocumentId(sidecar.document) && isRevision(sidecar.revision) && isValidXPointer(sidecar.xpointer) &&
         isValidPercentage(sidecar.percentage) &&
         (sidecar.basedOnCrosspoint.empty() || isRevision(sidecar.basedOnCrosspoint));
}

bool validateCrossPoint(const CrossPointSidecar& sidecar) {
  return isDocumentId(sidecar.document) && isRevision(sidecar.revision) && isValidXPointer(sidecar.xpointer) &&
         isValidPercentage(sidecar.percentage) &&
         (sidecar.appliedReadest.empty() || isRevision(sidecar.appliedReadest)) &&
         isBoundedPosition(sidecar.position);
}

bool getString(const JsonObjectConst object, const char* key, std::string& output, const size_t maximum) {
  const JsonVariantConst value = object[key];
  if (!value.is<const char*>()) return false;
  const char* string = value.as<const char*>();
  if (!string) return false;
  const size_t length = strlen(string);
  if (length > maximum) return false;
  output.assign(string, length);
  return true;
}

bool getOptionalRevision(const JsonObjectConst object, const char* key, std::string& output) {
  const JsonVariantConst value = object[key];
  if (value.isNull()) {
    output.clear();
    return true;
  }
  return getString(object, key, output, 32);
}

bool getInteger(const JsonObjectConst object, const char* key, int& output) {
  const JsonVariantConst value = object[key];
  if (!value.is<int>()) return false;
  output = value.as<int>();
  return true;
}

bool getPercentage(const JsonObjectConst object, float& output) {
  const JsonVariantConst value = object["percentage"];
  if (!value.is<float>()) return false;
  output = value.as<float>();
  return isValidPercentage(output);
}

bool setPercentage(JsonDocument& document, const float percentage) {
  // ArduinoJson intentionally stores a double that can be represented as a
  // float as a float, then serializes it with six decimal places. The sidecar
  // revision is defined over the original float bits, so write the shortest
  // safe nine-significant-digit JSON number directly instead.
  char text[16];
  const int length = snprintf(text, sizeof(text), "%.9g", static_cast<double>(percentage));
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(text)) return false;
  document["percentage"] = serialized(text, static_cast<size_t>(length));
  return true;
}

bool parseObject(const std::string_view json, JsonDocument& document, JsonObjectConst& object) {
  if (json.empty() || json.size() > MAX_SIDECAR_BYTES) return false;
  const DeserializationError error = deserializeJson(document, json.data(), json.size());
  if (error || !document.is<JsonObject>()) return false;
  object = document.as<JsonObjectConst>();
  return true;
}

}  // namespace

bool buildCrossPointRevisionInput(const std::string_view document, const std::string_view xpointer,
                                  const float percentage, std::string& input) {
  input.clear();
  if (!isDocumentId(document) || !isValidXPointer(xpointer) || !isValidPercentage(percentage)) return false;

  constexpr std::string_view domain = "crosspoint-progress-v2";
  input.reserve(domain.size() + document.size() + xpointer.size() + 7);
  input.append(domain);
  input.push_back('\0');
  input.append(document);
  input.push_back('\0');
  input.append(xpointer);
  input.push_back('\0');

  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(percentage));
  memcpy(&bits, &percentage, sizeof(bits));
  input.push_back(static_cast<char>(bits >> 24U));
  input.push_back(static_cast<char>(bits >> 16U));
  input.push_back(static_cast<char>(bits >> 8U));
  input.push_back(static_cast<char>(bits));
  return true;
}

bool matchesCrossPointRevision(const CrossPointSidecar& sidecar, const std::string_view calculatedRevision) {
  return validateCrossPoint(sidecar) && isRevision(calculatedRevision) && sidecar.revision == calculatedRevision;
}

CrossPointSidecar createCrossPointSidecar(const std::string_view document, const std::string_view revision,
                                          const std::string_view xpointer, const float percentage,
                                          const std::string_view appliedReadest,
                                          const LocalProgressPosition& position) {
  CrossPointSidecar sidecar;
  sidecar.document = document;
  sidecar.revision = revision;
  sidecar.xpointer = xpointer;
  sidecar.percentage = percentage;
  sidecar.appliedReadest = appliedReadest;
  sidecar.position = position;
  return sidecar;
}

std::string appliedRevisionForExit(const CrossPointSidecar* previous,
                                   const LocalProgressPosition& currentPosition) {
  if (previous && shouldPreserveAppliedRevision(previous->appliedReadest, previous->position, currentPosition)) {
    return previous->appliedReadest;
  }
  return {};
}

bool parseReadestSidecar(const std::string_view json, ReadestSidecar& sidecar) {
  sidecar = {};
  JsonDocument document;
  JsonObjectConst object;
  if (!parseObject(json, document, object) ||
      !hasKey(object, "schemaVersion") || !hasKey(object, "document") || !hasKey(object, "revision") ||
      !hasKey(object, "xpointer") || !hasKey(object, "percentage") || !hasKey(object, "basedOnCrosspoint") ||
      !object["schemaVersion"].is<int>() || object["schemaVersion"].as<int>() != SIDECAR_SCHEMA_VERSION ||
      !getString(object, "document", sidecar.document, 32) ||
      !getString(object, "revision", sidecar.revision, 32) ||
      !getString(object, "xpointer", sidecar.xpointer, MAX_XPOINTER_BYTES) ||
      !getPercentage(object, sidecar.percentage) ||
      !getOptionalRevision(object, "basedOnCrosspoint", sidecar.basedOnCrosspoint)) {
    return false;
  }
  return validateReadest(sidecar);
}

bool parseCrossPointSidecar(const std::string_view json, CrossPointSidecar& sidecar) {
  sidecar = {};
  JsonDocument document;
  JsonObjectConst object;
  if (!parseObject(json, document, object) ||
      !hasKey(object, "schemaVersion") || !hasKey(object, "document") || !hasKey(object, "revision") ||
      !hasKey(object, "xpointer") || !hasKey(object, "percentage") || !hasKey(object, "appliedReadest") ||
      !hasKey(object, "spineIndex") || !hasKey(object, "pageNumber") || !hasKey(object, "pageCount") ||
      !object["schemaVersion"].is<int>() || object["schemaVersion"].as<int>() != SIDECAR_SCHEMA_VERSION ||
      !getString(object, "document", sidecar.document, 32) ||
      !getString(object, "revision", sidecar.revision, 32) ||
      !getString(object, "xpointer", sidecar.xpointer, MAX_XPOINTER_BYTES) ||
      !getPercentage(object, sidecar.percentage) ||
      !getOptionalRevision(object, "appliedReadest", sidecar.appliedReadest) ||
      !getInteger(object, "spineIndex", sidecar.position.spineIndex) ||
      !getInteger(object, "pageNumber", sidecar.position.pageNumber) ||
      !getInteger(object, "pageCount", sidecar.position.pageCount)) {
    return false;
  }
  return validateCrossPoint(sidecar);
}

bool serializeReadestSidecar(const ReadestSidecar& sidecar, std::string& json) {
  json.clear();
  if (!validateReadest(sidecar)) return false;
  JsonDocument document;
  document["schemaVersion"] = SIDECAR_SCHEMA_VERSION;
  document["document"] = sidecar.document;
  document["revision"] = sidecar.revision;
  document["xpointer"] = sidecar.xpointer;
  if (!setPercentage(document, sidecar.percentage)) return false;
  if (sidecar.basedOnCrosspoint.empty())
    document["basedOnCrosspoint"] = nullptr;
  else
    document["basedOnCrosspoint"] = sidecar.basedOnCrosspoint;
  serializeJson(document, json);
  return json.size() <= MAX_SIDECAR_BYTES;
}

bool serializeCrossPointSidecar(const CrossPointSidecar& sidecar, std::string& json) {
  json.clear();
  if (!validateCrossPoint(sidecar)) return false;
  JsonDocument document;
  document["schemaVersion"] = SIDECAR_SCHEMA_VERSION;
  document["document"] = sidecar.document;
  document["revision"] = sidecar.revision;
  document["xpointer"] = sidecar.xpointer;
  if (!setPercentage(document, sidecar.percentage)) return false;
  if (sidecar.appliedReadest.empty())
    document["appliedReadest"] = nullptr;
  else
    document["appliedReadest"] = sidecar.appliedReadest;
  document["spineIndex"] = sidecar.position.spineIndex;
  document["pageNumber"] = sidecar.position.pageNumber;
  document["pageCount"] = sidecar.position.pageCount;
  serializeJson(document, json);
  return json.size() <= MAX_SIDECAR_BYTES;
}

std::string readestSidecarPath(const std::string_view document) {
  return isDocumentId(document) ? std::string(SIDECAR_DIRECTORY) + "/" + std::string(document) + ".readest.json" : "";
}

std::string crossPointSidecarPath(const std::string_view document) {
  return isDocumentId(document) ? std::string(SIDECAR_DIRECTORY) + "/" + std::string(document) + ".crosspoint.json"
                                : "";
}

}  // namespace ReadestProgress
