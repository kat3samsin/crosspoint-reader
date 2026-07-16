#include "ReadestProgressStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <AtomicFileReplace.h>
#include <FsHelpers.h>
#include <KOReaderDocumentId.h>
#include <ReadestLibraryOwnership.h>

#include <algorithm>
#include <functional>

#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "util/TaskWatchdog.h"

namespace ReadestProgressStore {
namespace {

struct StorageFileSystem {
  bool exists(const std::string& path) const { return Storage.exists(path.c_str()); }
  bool remove(const std::string& path) const { return Storage.remove(path.c_str()); }
  bool rename(const std::string& from, const std::string& to) const {
    return Storage.rename(from.c_str(), to.c_str());
  }
};

bool readBoundedFile(const std::string& path, std::string& json) {
  json.clear();
  HalFile file;
  if (!Storage.openFileForRead("RPS", path, file)) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > ReadestProgress::MAX_SIDECAR_BYTES) {
    file.close();
    return false;
  }
  json.resize(size);
  const int bytesRead = file.read(json.data(), size);
  file.close();
  return bytesRead == static_cast<int>(size);
}

bool ensureSidecarDirectory() {
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) return false;
  const std::string directory(ReadestProgress::SIDECAR_DIRECTORY);
  return Storage.exists(directory.c_str()) || Storage.mkdir(directory.c_str());
}

bool isReadableNonEmptyFile(const std::string& path) {
  HalFile file = Storage.open(path.c_str());
  if (!file) return false;
  const bool valid = !file.isDirectory() && file.fileSize() > 0;
  file.close();
  return valid;
}

bool writeAtomic(const std::string& path, const std::string& json) {
  const std::string temporaryPath = path + ".tmp";
  Storage.remove(temporaryPath.c_str());
  {
    HalFile file;
    if (!Storage.openFileForWrite("RPS", temporaryPath, file)) return false;
    const size_t written = file.write(json.data(), json.size());
    if (written != json.size()) {
      file.close();
      Storage.remove(temporaryPath.c_str());
      return false;
    }
    file.flush();
  }

  std::string verified;
  if (!readBoundedFile(temporaryPath, verified) || verified != json) return false;
  StorageFileSystem fileSystem;
  return AtomicFileReplace::replace(temporaryPath, path, path + ".bak", fileSystem);
}

bool isRootEpubPath(const std::string_view path) {
  return path.size() > 6 && path.front() == '/' && path.find('/', 1) == std::string_view::npos &&
         FsHelpers::hasEpubExtension(path);
}

std::string bookCachePath(const std::string& path) {
  return "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(path));
}

void migrateBookPathState(const std::string& oldPath, const std::string& newPath) {
  if (oldPath == newPath) return;

  const std::string oldCachePath = bookCachePath(oldPath);
  const std::string newCachePath = bookCachePath(newPath);
  if (Storage.exists(oldCachePath.c_str())) {
    if (!Storage.exists(newCachePath.c_str())) {
      if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
        LOG_ERR("RPS", "Failed to migrate book cache: %s", oldCachePath.c_str());
      }
    } else if (!Storage.removeDir(oldCachePath.c_str())) {
      LOG_ERR("RPS", "Failed to remove duplicate book cache: %s", oldCachePath.c_str());
    }
  }

  const auto& recentBooks = RECENT_BOOKS.getBooks();
  const bool canonicalAlreadyRecent =
      std::any_of(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == newPath; });
  if (canonicalAlreadyRecent) {
    RECENT_BOOKS.removeByPath(oldPath);
  } else {
    RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  }

  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

std::string calculateRevision(const std::string_view document, const std::string_view xpointer,
                              const float percentage) {
  std::string input;
  if (!ReadestProgress::buildCrossPointRevisionInput(document, xpointer, percentage, input)) return {};
  MD5Builder md5;
  md5.begin();
  md5.add(reinterpret_cast<const uint8_t*>(input.data()), input.size());
  md5.calculate();
  return md5.toString().c_str();
}

bool loadReadest(const std::string_view document, ReadestProgress::ReadestSidecar& sidecar) {
  const std::string path = ReadestProgress::readestSidecarPath(document);
  if (path.empty()) return false;
  StorageFileSystem fileSystem;
  const auto validator = [&](const std::string& candidate) {
    std::string candidateJson;
    ReadestProgress::ReadestSidecar candidateSidecar;
    return readBoundedFile(candidate, candidateJson) &&
           ReadestProgress::parseReadestSidecar(candidateJson, candidateSidecar) &&
           candidateSidecar.document == document;
  };
  const std::string recovered = AtomicFileReplace::recover(
      path, path + ".davbak", path + ".davtmp", fileSystem, validator);
  if (recovered.empty()) return false;
  std::string json;
  return readBoundedFile(recovered, json) && ReadestProgress::parseReadestSidecar(json, sidecar) &&
         sidecar.document == document;
}

bool loadCrossPoint(const std::string_view document, ReadestProgress::CrossPointSidecar& sidecar) {
  const std::string path = ReadestProgress::crossPointSidecarPath(document);
  if (path.empty()) return false;
  StorageFileSystem fileSystem;
  const auto validator = [&](const std::string& candidate) {
    std::string candidateJson;
    ReadestProgress::CrossPointSidecar candidateSidecar;
    return readBoundedFile(candidate, candidateJson) &&
           ReadestProgress::parseCrossPointSidecar(candidateJson, candidateSidecar) &&
           candidateSidecar.document == document &&
           ReadestProgress::matchesCrossPointRevision(
               candidateSidecar,
               calculateRevision(candidateSidecar.document, candidateSidecar.xpointer,
                                 candidateSidecar.percentage));
  };
  const std::string recovered = AtomicFileReplace::recover(
      path, path + ".bak", path + ".tmp", fileSystem, validator);
  if (recovered.empty()) return false;
  std::string json;
  if (!readBoundedFile(recovered, json) || !ReadestProgress::parseCrossPointSidecar(json, sidecar) ||
      sidecar.document != document) {
    return false;
  }
  return ReadestProgress::matchesCrossPointRevision(
      sidecar, calculateRevision(sidecar.document, sidecar.xpointer, sidecar.percentage));
}

bool saveCrossPoint(ReadestProgress::CrossPointSidecar& sidecar) {
  sidecar.revision = calculateRevision(sidecar.document, sidecar.xpointer, sidecar.percentage);
  std::string json;
  const std::string path = ReadestProgress::crossPointSidecarPath(sidecar.document);
  if (path.empty() || !ensureSidecarDirectory() || !ReadestProgress::serializeCrossPointSidecar(sidecar, json)) {
    return false;
  }
  if (!writeAtomic(path, json)) {
    LOG_ERR("RPS", "Failed to write CrossPoint progress sidecar: %s", path.c_str());
    return false;
  }
  return true;
}

ManifestOwnership getManifestOwnership(const std::string_view rootBookPath) {
  constexpr char manifestPath[] = "/.crosspoint/readest-library.json";
  constexpr char backupPath[] = "/.crosspoint/readest-library.json.davbak";
  constexpr char temporaryPath[] = "/.crosspoint/readest-library.json.davtmp";
  const bool hasManifestArtifact =
      Storage.exists(manifestPath) || Storage.exists(backupPath) || Storage.exists(temporaryPath);
  if (!hasManifestArtifact) return ManifestOwnership::NoManifest;

  StorageFileSystem fileSystem;
  const std::string recovered = AtomicFileReplace::recover(
      manifestPath, backupPath, temporaryPath, fileSystem, isReadableNonEmptyFile);
  if (recovered != manifestPath) return ManifestOwnership::Unknown;

  HalFile file;
  if (!Storage.openFileForRead("RPS", manifestPath, file)) return ManifestOwnership::Unknown;
  ReadestProgress::LibraryOwnershipScanner scanner(rootBookPath);
  char buffer[256];
  while (file.available() && !scanner.ownsBook() && !scanner.hasError()) {
    const int bytesRead = file.read(buffer, sizeof(buffer));
    if (bytesRead <= 0) {
      file.close();
      return ManifestOwnership::Unknown;
    }
    scanner.feed(buffer, static_cast<size_t>(bytesRead));
  }
  file.close();
  if (scanner.hasError()) return ManifestOwnership::Unknown;
  return scanner.ownsBook() ? ManifestOwnership::Owned : ManifestOwnership::Unowned;
}

void removeDuplicateRootBooks(const std::string_view canonicalPath) {
  if (!isRootEpubPath(canonicalPath)) return;
  if (getManifestOwnership(canonicalPath) != ManifestOwnership::Owned) return;

  const std::string requestedPath(canonicalPath);
  const std::string simplePath = ReadestProgress::canonicalRootBookPath(canonicalPath);
  const std::string sourcePath = Storage.exists(simplePath.c_str()) ? simplePath : requestedPath;
  if (!Storage.exists(sourcePath.c_str())) return;

  resetTaskWatchdogIfSubscribed();
  const std::string canonicalDocument = KOReaderDocumentId::calculate(sourcePath);
  if (canonicalDocument.empty()) return;

  bool canonicalReady = sourcePath == simplePath || Storage.exists(simplePath.c_str());
  if (sourcePath != simplePath && !canonicalReady) {
    canonicalReady = Storage.rename(sourcePath.c_str(), simplePath.c_str());
    if (canonicalReady) {
      migrateBookPathState(sourcePath, simplePath);
      LOG_INF("RPS", "Renamed Readest book to canonical path: %s", simplePath.c_str());
    }
  }
  if (!canonicalReady) return;

  HalFile root = Storage.open("/");
  if (!root || !root.isDirectory()) return;

  char name[500];
  for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(name, sizeof(name));
    const std::string candidatePath = std::string("/") + name;
    const bool candidateIsFile = !entry.isDirectory();
    entry.close();
    if (!candidateIsFile || candidatePath == simplePath || !isRootEpubPath(candidatePath)) continue;
    if (ReadestProgress::canonicalRootBookPath(candidatePath) != simplePath) continue;

    resetTaskWatchdogIfSubscribed();
    if (KOReaderDocumentId::calculate(candidatePath) == canonicalDocument) {
      migrateBookPathState(candidatePath, simplePath);
      LOG_INF("RPS", "Removing duplicate Readest book: %s (kept %s)", candidatePath.c_str(),
              simplePath.c_str());
      if (!Storage.remove(candidatePath.c_str())) {
        LOG_ERR("RPS", "Failed to remove duplicate Readest book: %s", candidatePath.c_str());
      }
    }
  }
  root.close();
}

}  // namespace ReadestProgressStore
