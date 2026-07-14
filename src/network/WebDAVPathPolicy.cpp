#include "WebDAVPathPolicy.h"

namespace {

bool isManifestOperationAllowed(const WebDAVOperation operation) {
  return operation == WebDAVOperation::Get || operation == WebDAVOperation::Head ||
         operation == WebDAVOperation::Put;
}

bool isReadOperation(const WebDAVOperation operation) {
  return operation == WebDAVOperation::Get || operation == WebDAVOperation::Head;
}

bool isLowercaseHex(const std::string_view value) {
  if (value.size() != 32) return false;
  for (const char character : value) {
    if (!((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'))) return false;
  }
  return true;
}

bool isReservedSegment(const std::string_view segment) {
  return segment == "System Volume Information" || segment == "XTCache";
}

}  // namespace

WebDAVPathPolicy::ProgressSidecarKind WebDAVPathPolicy::classifyProgressSidecar(
    const std::string_view path) {
  constexpr std::string_view prefix = "/.crosspoint/readest-sync/";
  constexpr std::string_view readestSuffix = ".readest.json";
  constexpr std::string_view crossPointSuffix = ".crosspoint.json";
  if (!path.starts_with(prefix)) return ProgressSidecarKind::None;

  const std::string_view filename = path.substr(prefix.size());
  if (filename.size() == 32 + readestSuffix.size() && filename.ends_with(readestSuffix) &&
      isLowercaseHex(filename.substr(0, 32))) {
    return ProgressSidecarKind::ReadestOwned;
  }
  if (filename.size() == 32 + crossPointSuffix.size() && filename.ends_with(crossPointSuffix) &&
      isLowercaseHex(filename.substr(0, 32))) {
    return ProgressSidecarKind::CrossPointOwned;
  }
  return ProgressSidecarKind::None;
}

bool WebDAVPathPolicy::isProtected(const std::string_view path,
                                   const WebDAVOperation operation) {
  if (path == READEST_LIBRARY_MANIFEST) return !isManifestOperationAllowed(operation);
  switch (classifyProgressSidecar(path)) {
    case ProgressSidecarKind::ReadestOwned:
      return !(isReadOperation(operation) || operation == WebDAVOperation::Put);
    case ProgressSidecarKind::CrossPointOwned:
      return !isReadOperation(operation);
    case ProgressSidecarKind::None:
      break;
  }

  std::size_t start = 0;
  while (start < path.size()) {
    while (start < path.size() && path[start] == '/') ++start;
    if (start == path.size()) break;

    const std::size_t separator = path.find('/', start);
    const std::size_t end = separator == std::string_view::npos ? path.size() : separator;
    const std::string_view segment = path.substr(start, end - start);

    if ((!segment.empty() && segment.front() == '.') || isReservedSegment(segment)) return true;
    start = end;
  }

  return false;
}
