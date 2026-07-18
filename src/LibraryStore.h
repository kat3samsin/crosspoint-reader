#pragma once

#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

struct LibraryBook {
  std::string path;
  std::string title;
  std::string author;
  uint64_t size = 0;

  bool operator==(const LibraryBook& other) const {
    return path == other.path && title == other.title && author == other.author && size == other.size;
  }
};

class LibraryStore : public PersistableStore<LibraryStore> {
 private:
  std::vector<LibraryBook> books;

  LibraryStore() = default;
  ~LibraryStore() = default;

  friend class PersistableStore<LibraryStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/library-cache.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  const std::vector<LibraryBook>& getBooks() const { return books; }
  void replaceBooks(std::vector<LibraryBook> updatedBooks);
};

#define LIBRARY_STORE LibraryStore::getInstance()
