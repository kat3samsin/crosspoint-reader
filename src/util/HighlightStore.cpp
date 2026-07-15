#include "HighlightStore.h"

#include <AtomicFileReplace.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "StringUtils.h"

namespace {

constexpr const char* HIGHLIGHTS_DIR = "/Highlights";
constexpr const char* SINGLE_FILE_PATH = "/Highlights.md";
constexpr const char* RANGE_DIR = "/.crosspoint/highlights";

// Last successful append, used to group consecutive passages under one
// heading without re-reading the file. Static (not per-activity) so the
// grouping survives re-entering the reader; a reboot just re-writes one
// heading, which markdown tolerates. Bounded by title lengths (~100 bytes).
std::string memoPath;
std::string memoBook;
std::string memoChapter;

uint32_t hashPath(const std::string& path) {
  uint32_t hash = 2166136261u;
  for (const unsigned char byte : path) {
    hash ^= byte;
    hash *= 16777619u;
  }
  return hash;
}

void appendHex32(std::string& output, const uint32_t value) {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (int shift = 28; shift >= 0; shift -= 4) output.push_back(HEX_DIGITS[(value >> shift) & 0x0f]);
}

struct StorageFileSystem {
  bool exists(const std::string& path) const { return Storage.exists(path.c_str()); }
  bool remove(const std::string& path) const { return Storage.remove(path.c_str()); }
  bool rename(const std::string& from, const std::string& to) const { return Storage.rename(from.c_str(), to.c_str()); }
};

std::string rangePath(const std::string& bookPath) {
  std::string name = bookPath;
  if (!name.empty() && name.front() == '/') name.erase(0, 1);
  std::replace(name.begin(), name.end(), '/', '_');
  std::replace(name.begin(), name.end(), '\\', '_');
  const size_t extension = name.find_last_of('.');
  if (extension != std::string::npos) name.erase(extension);
  name = StringUtils::sanitizeFilename(name, 80);
  name.push_back('-');
  appendHex32(name, hashPath(bookPath));
  return std::string(RANGE_DIR) + "/" + name + ".ranges";
}

bool readBoundedFile(const std::string& path, std::string& data) {
  data.clear();
  HalFile file;
  if (!Storage.openFileForRead("HILITE", path, file)) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > Highlights::MAX_RANGE_FILE_BYTES) {
    file.close();
    return false;
  }
  data.resize(size);
  const int bytesRead = file.read(data.data(), size);
  file.close();
  return bytesRead == static_cast<int>(size);
}

bool ensureRangeDirectory() {
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) return false;
  return Storage.exists(RANGE_DIR) || Storage.mkdir(RANGE_DIR);
}

bool writeRangesAtomic(const std::string& path, const std::string& data) {
  const std::string temporaryPath = path + ".tmp";
  Storage.remove(temporaryPath.c_str());
  {
    HalFile file;
    if (!Storage.openFileForWrite("HILITE", temporaryPath, file)) return false;
    const size_t written = file.write(data.data(), data.size());
    if (written != data.size()) {
      file.close();
      Storage.remove(temporaryPath.c_str());
      return false;
    }
    file.flush();
  }

  std::string verified;
  std::vector<Highlights::Range> parsed;
  if (!readBoundedFile(temporaryPath, verified) || verified != data || !Highlights::parseRanges(verified, parsed)) {
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  StorageFileSystem fileSystem;
  return AtomicFileReplace::replace(temporaryPath, path, path + ".bak", fileSystem);
}

bool appendMarkdown(const std::string& bookTitle, const std::string& chapterTitle, const std::string& passage) {
  if (passage.empty()) return false;
  const std::string book = bookTitle.empty() ? "Untitled" : bookTitle;

  const bool perBook = SETTINGS.highlightFileMode == CrossPointSettings::HIGHLIGHT_FILE_PER_BOOK;
  std::string path;
  if (perBook) {
    if (!Storage.ensureDirectoryExists(HIGHLIGHTS_DIR)) {
      LOG_ERR("HILITE", "Cannot create %s", HIGHLIGHTS_DIR);
      return false;
    }
    path = HIGHLIGHTS_DIR;
    path += '/';
    path += StringUtils::sanitizeFilename(book);
    path += ".md";
  } else {
    path = SINGLE_FILE_PATH;
  }

  HalFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("HILITE", "Cannot open %s", path.c_str());
    return false;
  }
  const bool fresh = file.fileSize() == 0;
  const bool sameBook = !fresh && path == memoPath && book == memoBook;
  const bool sameChapter = sameBook && chapterTitle == memoChapter;

  std::string out;
  out.reserve(book.size() + chapterTitle.size() + passage.size() + 16);
  if (fresh || (!perBook && !sameBook)) {
    if (!fresh) out += '\n';
    out += "# ";
    out += book;
    out += '\n';
  }
  if (!chapterTitle.empty() && !sameChapter) {
    out += "\n## ";
    out += chapterTitle;
    out += '\n';
  }
  out += "\n> ";
  out += passage;
  out += '\n';

  if (file.write(out.data(), out.size()) != out.size()) {
    LOG_ERR("HILITE", "Write failed: %s", path.c_str());
    return false;
  }
  memoPath = std::move(path);
  memoBook = book;
  memoChapter = chapterTitle;
  return true;
}

}  // namespace

namespace HighlightStore {

bool loadRanges(const std::string& bookPath, std::vector<Highlights::Range>& ranges) {
  ranges.clear();
  const std::string path = rangePath(bookPath);
  StorageFileSystem fileSystem;
  const bool hasArtifact =
      fileSystem.exists(path) || fileSystem.exists(path + ".bak") || fileSystem.exists(path + ".tmp");
  if (!hasArtifact) return true;

  const auto validator = [&](const std::string& candidate) {
    std::string data;
    std::vector<Highlights::Range> parsed;
    return readBoundedFile(candidate, data) && Highlights::parseRanges(data, parsed);
  };
  const std::string recovered = AtomicFileReplace::recover(path, path + ".bak", path + ".tmp", fileSystem, validator);
  if (recovered.empty()) return false;

  std::string data;
  return readBoundedFile(recovered, data) && Highlights::parseRanges(data, ranges);
}

bool save(const std::string& bookPath, const std::string& bookTitle, const std::string& chapterTitle,
          const std::string& passage, const Highlights::Range& range) {
  if (passage.empty() || !Highlights::isValid(range) || !ensureRangeDirectory()) return false;

  std::vector<Highlights::Range> ranges;
  if (!loadRanges(bookPath, ranges)) return false;
  const auto sameRange = [&](const Highlights::Range& existing) {
    return existing.spineIndex == range.spineIndex && existing.startWord == range.startWord &&
           existing.endWord == range.endWord;
  };
  if (std::any_of(ranges.begin(), ranges.end(), sameRange)) return true;
  if (ranges.size() >= Highlights::MAX_RANGES_PER_BOOK) return false;

  ranges.push_back(range);
  std::sort(ranges.begin(), ranges.end(), [](const Highlights::Range& a, const Highlights::Range& b) {
    if (a.spineIndex != b.spineIndex) return a.spineIndex < b.spineIndex;
    if (a.startWord != b.startWord) return a.startWord < b.startWord;
    return a.endWord < b.endWord;
  });

  std::string data;
  if (!Highlights::serializeRanges(ranges, data) || !writeRangesAtomic(rangePath(bookPath), data)) return false;
  if (!appendMarkdown(bookTitle, chapterTitle, passage)) {
    LOG_ERR("HILITE", "Saved persistent range but failed to append markdown clipping");
  }
  return true;
}

}  // namespace HighlightStore
