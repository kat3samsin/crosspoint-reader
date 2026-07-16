#include "ReadestLibraryOwnership.h"

#include <cstring>

#include <string>

namespace ReadestProgress {
namespace {

bool isSafeRootEpubPath(const std::string_view path) {
  if (path.size() < 7 || path.front() != '/' || path.find('/', 1) != std::string_view::npos) return false;
  constexpr std::string_view extension = ".epub";
  if (path.size() < extension.size()) return false;
  const std::string_view suffix = path.substr(path.size() - extension.size());
  for (size_t index = 0; index < extension.size(); index++) {
    char character = suffix[index];
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    if (character != extension[index]) return false;
  }
  return true;
}

bool isHex(const char character) {
  return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}

}  // namespace

std::string canonicalRootBookPath(const std::string_view path) {
  if (!isSafeRootEpubPath(path)) return std::string(path);

  constexpr size_t suffixLength = 7;
  constexpr size_t extensionLength = 5;
  const size_t stemEnd = path.size() - extensionLength;
  if (stemEnd <= suffixLength + 1 || path[stemEnd - suffixLength - 1] != '-') {
    return std::string(path);
  }

  const size_t hashStart = stemEnd - suffixLength;
  for (size_t index = hashStart; index < stemEnd; ++index) {
    if (!isHex(path[index])) return std::string(path);
  }

  std::string canonical(path.substr(0, hashStart - 1));
  canonical += ".epub";
  return canonical;
}

JsonCallbacks LibraryOwnershipScanner::callbacksFor(LibraryOwnershipScanner* scanner) {
  return {scanner, onKey, onString, onNumber, onBool, onNull, nullptr, nullptr, nullptr, nullptr};
}

LibraryOwnershipScanner::LibraryOwnershipScanner(const std::string_view rootBookPath)
    : targetPath(isSafeRootEpubPath(rootBookPath) ? canonicalRootBookPath(rootBookPath) : std::string{}),
      parser(callbacksFor(this)) {}

void LibraryOwnershipScanner::feed(const char* data, const size_t length) {
  if (!owned) parser.feed(data, length);
}

void LibraryOwnershipScanner::onKey(void* context, const char* key, const size_t length) {
  auto* scanner = static_cast<LibraryOwnershipScanner*>(context);
  scanner->awaitingPathValue = length == 4 && memcmp(key, "path", 4) == 0;
}

void LibraryOwnershipScanner::onString(void* context, const char* value, const size_t length) {
  auto* scanner = static_cast<LibraryOwnershipScanner*>(context);
  if (scanner->awaitingPathValue && !scanner->targetPath.empty() &&
      canonicalRootBookPath(std::string_view(value, length)) == scanner->targetPath) {
    scanner->owned = true;
  }
  scanner->awaitingPathValue = false;
}

void LibraryOwnershipScanner::onOtherValue(void* context) {
  static_cast<LibraryOwnershipScanner*>(context)->awaitingPathValue = false;
}

void LibraryOwnershipScanner::onNumber(void* context, const char*, size_t) { onOtherValue(context); }
void LibraryOwnershipScanner::onBool(void* context, bool) { onOtherValue(context); }
void LibraryOwnershipScanner::onNull(void* context) { onOtherValue(context); }

bool manifestOwnsRootBook(const std::string_view json, const std::string_view rootBookPath) {
  LibraryOwnershipScanner scanner(rootBookPath);
  scanner.feed(json.data(), json.size());
  return !scanner.hasError() && scanner.ownsBook();
}

}  // namespace ReadestProgress
