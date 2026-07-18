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

}  // namespace

JsonCallbacks LibraryOwnershipScanner::callbacksFor(LibraryOwnershipScanner* scanner) {
  return {scanner, onKey, onString, onNumber, onBool, onNull, nullptr, nullptr, nullptr, nullptr};
}

LibraryOwnershipScanner::LibraryOwnershipScanner(const std::string_view rootBookPath)
    : targetPath(isSafeRootEpubPath(rootBookPath) ? rootBookPath : std::string{}),
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
  const std::string_view path(value, length);
  if (scanner->awaitingPathValue && !scanner->targetPath.empty() && isSafeRootEpubPath(path) &&
      path == scanner->targetPath) {
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
