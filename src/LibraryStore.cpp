#include "LibraryStore.h"

#include <Logging.h>

#include <utility>

void LibraryStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : books) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["size"] = book.size;
  }
}

bool LibraryStore::fromJson(JsonVariantConst doc) {
  books.clear();
  const JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  books.reserve(arr.size());
  for (const JsonObjectConst obj : arr) {
    LibraryBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.size = obj["size"] | static_cast<uint64_t>(0);
    if (!book.path.empty()) books.push_back(std::move(book));
  }
  LOG_DBG("LIB", "Library cache loaded (%d entries)", static_cast<int>(books.size()));
  return true;
}

void LibraryStore::replaceBooks(std::vector<LibraryBook> updatedBooks) {
  if (books == updatedBooks) return;
  books = std::move(updatedBooks);
  if (!saveToFile()) {
    LOG_ERR("LIB", "Failed to persist library cache");
  }
}
