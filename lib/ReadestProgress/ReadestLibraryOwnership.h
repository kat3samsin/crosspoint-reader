#pragma once

#include <StreamingJsonParser.h>

#include <string>
#include <string_view>

namespace ReadestProgress {

// Readest's WebDAV book names may end in "-<7 hex digits>" before .epub.
// CrossPoint stores the simpler title-only path while treating both forms as
// the same manifest-owned book.
std::string canonicalRootBookPath(std::string_view path);

class LibraryOwnershipScanner final {
  std::string targetPath;
  bool awaitingPathValue = false;
  bool owned = false;
  StreamingJsonParser parser;

  static JsonCallbacks callbacksFor(LibraryOwnershipScanner* scanner);
  static void onKey(void* context, const char* key, size_t length);
  static void onString(void* context, const char* value, size_t length);
  static void onOtherValue(void* context);
  static void onNumber(void* context, const char*, size_t);
  static void onBool(void* context, bool);
  static void onNull(void* context);

 public:
  explicit LibraryOwnershipScanner(std::string_view rootBookPath);
  void feed(const char* data, size_t length);
  bool ownsBook() const { return owned; }
  bool hasError() const { return parser.hasError(); }
};

bool manifestOwnsRootBook(std::string_view json, std::string_view rootBookPath);

}  // namespace ReadestProgress
