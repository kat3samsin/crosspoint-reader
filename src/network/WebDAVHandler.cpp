#include "WebDAVHandler.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <ReadestProgressSidecar.h>
#include <AtomicFileReplace.h>
#include "../activities/reader/ReadestProgressStore.h"
#include "util/BookCacheUtils.h"
#include "util/TaskWatchdog.h"

namespace {
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr unsigned long CLIENT_STALL_TIMEOUT_MS = 3000;

struct WebDAVStorageFileSystem {
  bool exists(const std::string& path) const { return Storage.exists(path.c_str()); }
  bool remove(const std::string& path) const { return Storage.remove(path.c_str()); }
  bool rename(const std::string& from, const std::string& to) const {
    return Storage.rename(from.c_str(), to.c_str());
  }
};

bool isReadableNonEmptyFile(const std::string& path) {
  HalFile file = Storage.open(path.c_str());
  if (!file) return false;
  const bool valid = !file.isDirectory() && file.fileSize() > 0;
  file.close();
  return valid;
}

bool isReadestLibraryManifest(const String& path) {
  return std::string_view(path.c_str(), path.length()) == WebDAVPathPolicy::READEST_LIBRARY_MANIFEST;
}

bool isRootEpubPath(const String& path) {
  const std::string_view value(path.c_str(), path.length());
  constexpr std::string_view extension = ".epub";
  if (value.size() <= extension.size() || value.front() != '/' ||
      value.find('/', 1) != std::string_view::npos) {
    return false;
  }
  const std::string_view suffix = value.substr(value.size() - extension.size());
  for (size_t index = 0; index < extension.size(); ++index) {
    char character = suffix[index];
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    if (character != extension[index]) return false;
  }
  return true;
}

bool isReadestManagedBook(const String& path) {
  return isRootEpubPath(path) &&
         ReadestProgressStore::getManifestOwnership(path.c_str()) ==
             ReadestProgressStore::ManifestOwnership::Owned;
}

bool writeChunk(NetworkClient& client, const uint8_t* data, const size_t length, size_t& written) {
  written = 0;
  const int socket = client.fd();
  if (socket < 0) return false;

  unsigned long lastProgress = millis();
  while (written < length) {
    const int result = send(socket, data + written, length - written, MSG_DONTWAIT);
    if (result > 0) {
      written += static_cast<size_t>(result);
      lastProgress = millis();
      continue;
    }
    if (result == 0) return false;
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
    if (millis() - lastProgress >= CLIENT_STALL_TIMEOUT_MS) return false;

    resetTaskWatchdogIfSubscribed();
    delay(1);
  }
  return true;
}

// RFC 1123 date format helper: "Sun, 06 Nov 1994 08:49:37 GMT"
// ESP32 doesn't have real-time clock set by default, so we use a fixed epoch date
// as a fallback. The date is not critical for WebDAV Class 1 operations.
const char* FIXED_DATE = "Thu, 01 Jan 2024 00:00:00 GMT";
}  // namespace

// ── RequestHandler interface ─────────────────────────────────────────────────

bool WebDAVHandler::canHandle(WebServer& server, HTTPMethod method, const String& uri) {
  (void)server;
  (void)uri;
  switch (method) {
    case HTTP_OPTIONS:
    case HTTP_PROPFIND:
    case HTTP_GET:
    case HTTP_HEAD:
    case HTTP_PUT:
    case HTTP_DELETE:
    case HTTP_MKCOL:
    case HTTP_MOVE:
    case HTTP_COPY:
    case HTTP_LOCK:
    case HTTP_UNLOCK:
      return true;
    default:
      return false;
  }
}

bool WebDAVHandler::canRaw(WebServer& server, const String& uri) {
  (void)uri;
  return server.method() == HTTP_PUT;
}

void WebDAVHandler::raw(WebServer& server, const String& uri, HTTPRaw& raw) {
  (void)uri;
  if (raw.status == RAW_START) {
    _putPath = getRequestPath(server);
    _putBytes = 0;
    _putBufferSize = 0;
    _putValidated = false;
    if (isProtectedPath(_putPath, WebDAVOperation::Put)) {
      _putOk = false;
      return;
    }

    // Create the parents for the exact Readest-owned hidden files internally;
    // MKCOL remains forbidden for every hidden path.
    const auto progressKind = WebDAVPathPolicy::classifyProgressSidecar(_putPath.c_str());
    const bool needsCrossPointDirectory =
        std::string_view(_putPath.c_str()) == WebDAVPathPolicy::READEST_LIBRARY_MANIFEST ||
        progressKind == WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned;
    if (needsCrossPointDirectory && !Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) {
      _putOk = false;
      return;
    }
    if (progressKind == WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned &&
        !Storage.exists(WebDAVPathPolicy::READEST_PROGRESS_DIRECTORY.data()) &&
        !Storage.mkdir(WebDAVPathPolicy::READEST_PROGRESS_DIRECTORY.data())) {
      _putOk = false;
      return;
    }

    // Ensure parent directory exists
    int lastSlash = _putPath.lastIndexOf('/');
    if (lastSlash > 0) {
      String parentPath = _putPath.substring(0, lastSlash);
      if (!Storage.exists(parentPath.c_str())) {
        _putOk = false;
        return;
      }
    }

    if (_putFile) _putFile.close();
    _putExisted = Storage.exists(_putPath.c_str());

    if (_putExisted) {
      HalFile existing = Storage.open(_putPath.c_str());
      if (existing && existing.isDirectory()) {
        existing.close();
        _putOk = false;
        return;
      }
      if (existing) existing.close();
    }

    // Write to a temp file to avoid destroying the original on failed upload
    String tempPath = _putPath + ".davtmp";
    Storage.remove(tempPath.c_str());
    _putOk = Storage.openFileForWrite("DAV", tempPath, _putFile);
    LOG_DBG("DAV", "PUT START: %s", _putPath.c_str());

  } else if (raw.status == RAW_WRITE) {
    if (_putFile && _putOk) {
      resetTaskWatchdogIfSubscribed();
      const bool isReadestProgressSidecar =
          WebDAVPathPolicy::classifyProgressSidecar(_putPath.c_str()) ==
          WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned;
      if (isReadestProgressSidecar &&
          (_putBytes > WebDAVPathPolicy::MAX_READEST_PROGRESS_SIDECAR_BYTES ||
           raw.currentSize > WebDAVPathPolicy::MAX_READEST_PROGRESS_SIDECAR_BYTES - _putBytes)) {
        _putOk = false;
        return;
      }

      const uint8_t* data = raw.buf;
      size_t remaining = raw.currentSize;
      while (remaining > 0 && _putOk) {
        const size_t space = _transferBuffer.size() - _putBufferSize;
        const size_t toCopy = std::min(remaining, space);
        std::memcpy(_transferBuffer.data() + _putBufferSize, data, toCopy);
        _putBufferSize += toCopy;
        data += toCopy;
        remaining -= toCopy;

        if (_putBufferSize == _transferBuffer.size() && !flushPutBuffer()) {
          _putOk = false;
        }
      }
      if (_putOk) _putBytes += raw.currentSize;
    }

  } else if (raw.status == RAW_END) {
    if (_putFile && _putOk) {
      _putOk = flushPutBuffer();
    } else {
      _putBufferSize = 0;
    }
    if (_putFile) _putFile.close();
    if (_putOk) {
      String tempPath = _putPath + ".davtmp";
      if (WebDAVPathPolicy::classifyProgressSidecar(_putPath.c_str()) ==
              WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned &&
          !validateProgressSidecarFile(_putPath, tempPath)) {
        _putOk = false;
      } else if (WebDAVPathPolicy::classifyProgressSidecar(_putPath.c_str()) ==
                 WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned) {
        _putValidated = true;
      }
    }
    if (_putOk) {
      String tempPath = _putPath + ".davtmp";
      const auto progressKind = WebDAVPathPolicy::classifyProgressSidecar(_putPath.c_str());
      if (progressKind == WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned ||
          isReadestLibraryManifest(_putPath) || isReadestManagedBook(_putPath)) {
        WebDAVStorageFileSystem fileSystem;
        _putOk = AtomicFileReplace::replace(
            tempPath.c_str(), _putPath.c_str(), std::string(_putPath.c_str()) + ".davbak", fileSystem);
      } else {
        if (_putExisted) Storage.remove(_putPath.c_str());
        HalFile tmp = Storage.open(tempPath.c_str());
        if (tmp) {
          _putOk = tmp.rename(_putPath.c_str());
          tmp.close();
        } else {
          _putOk = false;
        }
      }
    }
    if (!_putOk && !_putValidated) {
      String tempPath = _putPath + ".davtmp";
      Storage.remove(tempPath.c_str());
    }
    LOG_DBG("DAV", "PUT END: %u bytes, ok=%d", raw.totalSize, _putOk);

  } else if (raw.status == RAW_ABORTED) {
    _putBufferSize = 0;
    if (_putFile) _putFile.close();
    String tempPath = _putPath + ".davtmp";
    Storage.remove(tempPath.c_str());
    _putOk = false;
  }
}

bool WebDAVHandler::flushPutBuffer() {
  if (_putBufferSize == 0) return true;
  if (!_putFile) {
    _putBufferSize = 0;
    return false;
  }

  esp_task_wdt_reset();
  const size_t buffered = _putBufferSize;
  const size_t written = _putFile.write(_transferBuffer.data(), buffered);
  esp_task_wdt_reset();
  _putBufferSize = 0;
  return written == buffered;
}

bool WebDAVHandler::validateProgressSidecarFile(const String& targetPath, const String& candidatePath) {
  HalFile file;
  if (!Storage.openFileForRead("DAV", candidatePath, file)) return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > WebDAVPathPolicy::MAX_READEST_PROGRESS_SIDECAR_BYTES) {
    file.close();
    return false;
  }
  const int bytesRead = file.read(_transferBuffer.data(), size);
  file.close();
  if (bytesRead != static_cast<int>(size)) return false;

  const auto kind = WebDAVPathPolicy::classifyProgressSidecar(targetPath.c_str());
  std::string document;
  const std::string_view json(reinterpret_cast<const char*>(_transferBuffer.data()), size);
  if (kind == WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned) {
    ReadestProgress::ReadestSidecar sidecar;
    if (!ReadestProgress::parseReadestSidecar(json, sidecar)) return false;
    document = std::move(sidecar.document);
  } else if (kind == WebDAVPathPolicy::ProgressSidecarKind::CrossPointOwned) {
    ReadestProgress::CrossPointSidecar sidecar;
    if (!ReadestProgress::parseCrossPointSidecar(json, sidecar)) return false;
    if (!ReadestProgress::matchesCrossPointRevision(
            sidecar, ReadestProgressStore::calculateRevision(sidecar.document, sidecar.xpointer,
                                                              sidecar.percentage))) {
      return false;
    }
    document = std::move(sidecar.document);
  } else {
    return false;
  }

  constexpr size_t prefixLength = sizeof("/.crosspoint/readest-sync/") - 1;
  const std::string_view path(targetPath.c_str(), targetPath.length());
  return path.size() >= prefixLength + 32 && path.substr(prefixLength, 32) == document;
}

void WebDAVHandler::recoverProgressSidecar(const String& path) {
  const auto kind = WebDAVPathPolicy::classifyProgressSidecar(path.c_str());
  const bool isManifest = isReadestLibraryManifest(path);
  const bool isManagedBook = isReadestManagedBook(path);
  if (kind == WebDAVPathPolicy::ProgressSidecarKind::None && !isManifest && !isManagedBook) return;
  WebDAVStorageFileSystem fileSystem;
  const std::string finalPath(path.c_str());
  if (isManifest) {
    AtomicFileReplace::recover(finalPath, finalPath + ".davbak", finalPath + ".davtmp", fileSystem,
                               isReadableNonEmptyFile);
    return;
  }
  if (isManagedBook) {
    const std::string backupPath = finalPath + ".davbak";
    // A book payload is not self-describing enough to validate a temporary
    // file here. After an interrupted replacement, prefer the last known
    // readable copy and let Readest retry the still-uploading manifest entry.
    if (!Storage.exists(path.c_str()) && Storage.exists(backupPath.c_str())) {
      Storage.rename(backupPath.c_str(), path.c_str());
    }
    return;
  }
  const std::string backupPath =
      finalPath + (kind == WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned ? ".davbak" : ".bak");
  const std::string temporaryPath =
      finalPath + (kind == WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned ? ".davtmp" : ".tmp");
  AtomicFileReplace::recover(finalPath, backupPath, temporaryPath, fileSystem,
                             [&](const std::string& candidate) {
                               return validateProgressSidecarFile(path, candidate.c_str());
                             });
}

bool WebDAVHandler::handle(WebServer& server, HTTPMethod method, const String& uri) {
  (void)uri;
  switch (method) {
    case HTTP_OPTIONS:
      handleOptions(server);
      return true;
    case HTTP_PROPFIND:
      handlePropfind(server);
      return true;
    case HTTP_GET:
      handleGet(server);
      return true;
    case HTTP_HEAD:
      handleHead(server);
      return true;
    case HTTP_PUT:
      handlePut(server);
      return true;
    case HTTP_DELETE:
      handleDelete(server);
      return true;
    case HTTP_MKCOL:
      handleMkcol(server);
      return true;
    case HTTP_MOVE:
      handleMove(server);
      return true;
    case HTTP_COPY:
      handleCopy(server);
      return true;
    case HTTP_LOCK:
      handleLock(server);
      return true;
    case HTTP_UNLOCK:
      handleUnlock(server);
      return true;
    default:
      return false;
  }
}

// ── OPTIONS ──────────────────────────────────────────────────────────────────

void WebDAVHandler::handleOptions(WebServer& s) {
  s.sendHeader("DAV", "1");
  s.sendHeader("Allow",
               "OPTIONS, GET, HEAD, PUT, DELETE, "
               "PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK");
  s.sendHeader("MS-Author-Via", "DAV");
  s.send(200);
  LOG_DBG("DAV", "OPTIONS %s", s.uri().c_str());
}

// ── PROPFIND ─────────────────────────────────────────────────────────────────

void WebDAVHandler::handlePropfind(WebServer& s) {
  String path = getRequestPath(s);
  int depth = getDepth(s);

  LOG_DBG("DAV", "PROPFIND %s depth=%d", path.c_str(), depth);

  if (isProtectedPath(path, WebDAVOperation::Propfind)) {
    s.send(403, "text/plain", "Forbidden");
    return;
  }

  // Check if path exists
  if (!Storage.exists(path.c_str()) && path != "/") {
    s.send(404, "text/plain", "Not Found");
    return;
  }

  HalFile root = Storage.open(path.c_str());
  if (!root) {
    if (path == "/") {
      // Root should always work — send minimal response
      s.setContentLength(CONTENT_LENGTH_UNKNOWN);
      s.send(207, "application/xml; charset=\"utf-8\"", "");
      s.sendContent(
          "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
          "<D:multistatus xmlns:D=\"DAV:\">\n");
      sendPropEntry(s, "/", true, 0, FIXED_DATE);
      s.sendContent("</D:multistatus>\n");
      s.sendContent("");
      return;
    }
    s.send(500, "text/plain", "Failed to open");
    return;
  }

  bool isDir = root.isDirectory();

  s.setContentLength(CONTENT_LENGTH_UNKNOWN);
  s.send(207, "application/xml; charset=\"utf-8\"", "");
  s.sendContent(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<D:multistatus xmlns:D=\"DAV:\">\n");

  // Entry for the resource itself
  if (isDir) {
    sendPropEntry(s, path, true, 0, FIXED_DATE);
  } else {
    sendPropEntry(s, path, false, root.size(), FIXED_DATE);
    root.close();
    s.sendContent("</D:multistatus>\n");
    s.sendContent("");
    return;
  }

  // If depth > 0 and it's a directory, list children
  if (depth > 0) {
    HalFile file = root.openNextFile();
    char name[500];
    while (file) {
      file.getName(name, sizeof(name));
      String fileName(name);

      // Skip hidden/protected items
      bool shouldHide = fileName.startsWith(".");
      if (!shouldHide) {
        for (const auto* item : HIDDEN_ITEMS) {
          if (fileName.equals(item)) {
            shouldHide = true;
            break;
          }
        }
      }

      if (!shouldHide) {
        String childPath = path;
        if (!childPath.endsWith("/")) childPath += "/";
        childPath += fileName;

        if (file.isDirectory()) {
          sendPropEntry(s, childPath, true, 0, FIXED_DATE);
        } else {
          sendPropEntry(s, childPath, false, file.size(), FIXED_DATE);
        }
      }

      file.close();
      yield();
      resetTaskWatchdogIfSubscribed();
      file = root.openNextFile();
    }
  }

  root.close();
  s.sendContent("</D:multistatus>\n");
  s.sendContent("");
}

void WebDAVHandler::sendPropEntry(WebServer& s, const String& path, bool isDir, size_t size,
                                  const String& lastModified) const {
  String href;
  urlEncodePath(path, href);
  // Ensure directory hrefs end with /
  if (isDir && !href.endsWith("/")) href += "/";

  String xml = "<D:response><D:href>";
  xml += href;
  xml += "</D:href><D:propstat><D:prop>";

  if (isDir) {
    xml += "<D:resourcetype><D:collection/></D:resourcetype>";
  } else {
    xml += "<D:resourcetype/>";
    xml += "<D:getcontentlength>";
    xml += String(size);
    xml += "</D:getcontentlength>";
    String mime = getMimeType(path);
    xml += "<D:getcontenttype>";
    xml += mime;
    xml += "</D:getcontenttype>";
  }

  xml += "<D:getlastmodified>";
  xml += lastModified;
  xml += "</D:getlastmodified>";

  xml += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>\n";

  s.sendContent(xml);
}

// ── GET ──────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleGet(WebServer& s) {
  String path = getRequestPath(s);
  LOG_DBG("DAV", "GET %s", path.c_str());

  if (isProtectedPath(path, WebDAVOperation::Get)) {
    s.send(403, "text/plain", "Forbidden");
    return;
  }

  recoverProgressSidecar(path);

  if (!Storage.exists(path.c_str())) {
    s.send(404, "text/plain", "Not Found");
    return;
  }

  HalFile file = Storage.open(path.c_str());
  if (!file) {
    s.send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    // For directories, return a PROPFIND-like response or redirect
    s.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  String contentType = getMimeType(path);
  const size_t expectedBytes = file.size();
  s.setContentLength(expectedBytes);
  s.send(200, contentType.c_str(), "");

  NetworkClient client = s.client();
  size_t sentBytes = 0;
  bool downloadOk = true;

  // HalFile derives from Print, not Stream. Passing it to client.write()
  // converts it to bool and sends one byte while Content-Length advertises
  // the entire file. Stream explicit chunks, while bounding a stalled socket
  // so a disconnected sync client cannot freeze the foreground server.
  while (downloadOk && sentBytes < expectedBytes) {
    resetTaskWatchdogIfSubscribed();
    const size_t remaining = expectedBytes - sentBytes;
    const size_t requested = std::min(remaining, _transferBuffer.size());
    const int result = file.read(_transferBuffer.data(), requested);
    if (result <= 0) {
      downloadOk = false;
      break;
    }

    const size_t bytesRead = static_cast<size_t>(result);
    size_t chunkWritten = 0;
    downloadOk = writeChunk(client, _transferBuffer.data(), bytesRead, chunkWritten);
    sentBytes += chunkWritten;
  }

  file.close();
  if (!downloadOk || sentBytes != expectedBytes) {
    LOG_ERR("DAV", "GET interrupted: %s (%u/%u bytes)", path.c_str(), static_cast<unsigned>(sentBytes),
            static_cast<unsigned>(expectedBytes));
    client.stop();
  }
}

// ── HEAD ─────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleHead(WebServer& s) {
  String path = getRequestPath(s);
  LOG_DBG("DAV", "HEAD %s", path.c_str());

  if (isProtectedPath(path, WebDAVOperation::Head)) {
    s.send(403, "text/plain", "");
    return;
  }

  recoverProgressSidecar(path);

  if (!Storage.exists(path.c_str())) {
    s.send(404, "text/plain", "");
    return;
  }

  HalFile file = Storage.open(path.c_str());
  if (!file) {
    s.send(500, "text/plain", "");
    return;
  }

  if (file.isDirectory()) {
    file.close();
    s.send(200, "text/html", "");
    return;
  }

  String contentType = getMimeType(path);
  s.setContentLength(file.size());
  s.send(200, contentType.c_str(), "");
  file.close();
}

// ── PUT ──────────────────────────────────────────────────────────────────────

void WebDAVHandler::handlePut(WebServer& s) {
  // Body was already received via canRaw/raw callbacks
  String path = getRequestPath(s);
  LOG_DBG("DAV", "PUT %s", path.c_str());

  if (isProtectedPath(path, WebDAVOperation::Put)) {
    s.send(403, "text/plain", "Forbidden");
    return;
  }

  if (!_putOk) {
    if (!_putValidated) {
      String tempPath = path + ".davtmp";
      Storage.remove(tempPath.c_str());
    }
    s.send(500, "text/plain", "Write failed - incomplete upload or disk full");
    return;
  }

  clearBookCache(path.c_str());
  s.send(_putExisted ? 204 : 201);
  LOG_DBG("DAV", "PUT complete: %s", path.c_str());
}

// ── DELETE ───────────────────────────────────────────────────────────────────

void WebDAVHandler::handleDelete(WebServer& s) {
  String path = getRequestPath(s);
  LOG_DBG("DAV", "DELETE %s", path.c_str());

  if (path == "/" || path.isEmpty()) {
    s.send(403, "text/plain", "Cannot delete root");
    return;
  }

  if (isProtectedPath(path, WebDAVOperation::Delete)) {
    s.send(403, "text/plain", "Forbidden");
    return;
  }

  if (!Storage.exists(path.c_str())) {
    s.send(404, "text/plain", "Not Found");
    return;
  }

  HalFile file = Storage.open(path.c_str());
  if (!file) {
    s.send(500, "text/plain", "Failed to open");
    return;
  }

  if (file.isDirectory()) {
    // Check if directory is empty
    HalFile entry = file.openNextFile();
    if (entry) {
      entry.close();
      file.close();
      s.send(409, "text/plain", "Directory not empty");
      return;
    }
    file.close();
    if (Storage.rmdir(path.c_str())) {
      s.send(204);
    } else {
      s.send(500, "text/plain", "Failed to remove directory");
    }
  } else {
    file.close();
    clearBookCache(path.c_str());
    if (Storage.remove(path.c_str())) {
      s.send(204);
    } else {
      s.send(500, "text/plain", "Failed to delete file");
    }
  }
}

// ── MKCOL ────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleMkcol(WebServer& s) {
  String path = getRequestPath(s);
  LOG_DBG("DAV", "MKCOL %s", path.c_str());

  if (isProtectedPath(path, WebDAVOperation::Mkcol)) {
    s.send(403, "text/plain", "Forbidden");
    return;
  }

  // MKCOL must not have a body (RFC 4918)
  if (s.clientContentLength() > 0) {
    s.send(415, "text/plain", "Unsupported Media Type");
    return;
  }

  if (Storage.exists(path.c_str())) {
    s.send(405, "text/plain", "Already exists");
    return;
  }

  // Check parent exists
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash > 0) {
    String parentPath = path.substring(0, lastSlash);
    if (!parentPath.isEmpty() && !Storage.exists(parentPath.c_str())) {
      s.send(409, "text/plain", "Parent directory does not exist");
      return;
    }
  }

  if (Storage.mkdir(path.c_str())) {
    s.send(201);
    LOG_DBG("DAV", "Created directory: %s", path.c_str());
  } else {
    s.send(500, "text/plain", "Failed to create directory");
  }
}

// ── MOVE ─────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleMove(WebServer& s) {
  String srcPath = getRequestPath(s);
  String dstPath = getDestinationPath(s);
  bool overwrite = getOverwrite(s);

  LOG_DBG("DAV", "MOVE %s -> %s (overwrite=%d)", srcPath.c_str(), dstPath.c_str(), overwrite);

  if (srcPath == "/" || srcPath.isEmpty()) {
    s.send(403, "text/plain", "Cannot move root");
    return;
  }

  if (isProtectedPath(srcPath, WebDAVOperation::Move) ||
      isProtectedPath(dstPath, WebDAVOperation::Move)) {
    s.send(403, "text/plain", "Forbidden");
    return;
  }

  if (dstPath.isEmpty()) {
    s.send(400, "text/plain", "Missing Destination header");
    return;
  }

  if (srcPath == dstPath) {
    s.send(204);
    return;
  }

  if (!Storage.exists(srcPath.c_str())) {
    s.send(404, "text/plain", "Source not found");
    return;
  }

  // Check destination parent exists
  int lastSlash = dstPath.lastIndexOf('/');
  if (lastSlash > 0) {
    String parentPath = dstPath.substring(0, lastSlash);
    if (!parentPath.isEmpty() && !Storage.exists(parentPath.c_str())) {
      s.send(409, "text/plain", "Destination parent does not exist");
      return;
    }
  }

  bool dstExists = Storage.exists(dstPath.c_str());
  if (dstExists && !overwrite) {
    s.send(412, "text/plain", "Destination exists and Overwrite is F");
    return;
  }

  if (dstExists) {
    Storage.remove(dstPath.c_str());
  }

  HalFile file = Storage.open(srcPath.c_str());
  if (!file) {
    s.send(500, "text/plain", "Failed to open source");
    return;
  }

  clearBookCache(srcPath.c_str());
  bool success = file.rename(dstPath.c_str());
  file.close();

  if (success) {
    s.send(dstExists ? 204 : 201);
  } else {
    s.send(500, "text/plain", "Move failed");
  }
}

// ── COPY ─────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleCopy(WebServer& s) {
  String srcPath = getRequestPath(s);
  String dstPath = getDestinationPath(s);
  bool overwrite = getOverwrite(s);

  LOG_DBG("DAV", "COPY %s -> %s (overwrite=%d)", srcPath.c_str(), dstPath.c_str(), overwrite);

  if (isProtectedPath(srcPath, WebDAVOperation::Copy) ||
      isProtectedPath(dstPath, WebDAVOperation::Copy)) {
    s.send(403, "text/plain", "Forbidden");
    return;
  }

  if (dstPath.isEmpty()) {
    s.send(400, "text/plain", "Missing Destination header");
    return;
  }

  if (srcPath == dstPath) {
    s.send(204);
    return;
  }

  if (!Storage.exists(srcPath.c_str())) {
    s.send(404, "text/plain", "Source not found");
    return;
  }

  HalFile srcFile = Storage.open(srcPath.c_str());
  if (!srcFile) {
    s.send(500, "text/plain", "Failed to open source");
    return;
  }

  if (srcFile.isDirectory()) {
    srcFile.close();
    s.send(403, "text/plain", "Cannot copy directories");
    return;
  }

  // Check destination parent exists
  int lastSlash = dstPath.lastIndexOf('/');
  if (lastSlash > 0) {
    String parentPath = dstPath.substring(0, lastSlash);
    if (!parentPath.isEmpty() && !Storage.exists(parentPath.c_str())) {
      srcFile.close();
      s.send(409, "text/plain", "Destination parent does not exist");
      return;
    }
  }

  bool dstExists = Storage.exists(dstPath.c_str());
  if (dstExists && !overwrite) {
    srcFile.close();
    s.send(412, "text/plain", "Destination exists and Overwrite is F");
    return;
  }

  if (dstExists) {
    Storage.remove(dstPath.c_str());
  }

  HalFile dstFile;
  if (!Storage.openFileForWrite("DAV", dstPath, dstFile)) {
    srcFile.close();
    s.send(500, "text/plain", "Failed to create destination");
    return;
  }

  // Streaming copy with 4KB buffer on stack
  uint8_t buf[4096];
  bool copyOk = true;
  while (srcFile.available()) {
    resetTaskWatchdogIfSubscribed();
    int bytesRead = srcFile.read(buf, sizeof(buf));
    if (bytesRead <= 0) break;
    size_t written = dstFile.write(buf, bytesRead);
    if (written != (size_t)bytesRead) {
      copyOk = false;
      break;
    }
  }

  srcFile.close();
  dstFile.close();

  if (copyOk) {
    s.send(dstExists ? 204 : 201);
  } else {
    Storage.remove(dstPath.c_str());
    s.send(500, "text/plain", "Copy failed - disk full?");
  }
}

// ── LOCK / UNLOCK (dummy for client compatibility) ───────────────────────────

void WebDAVHandler::handleLock(WebServer& s) {
  String path = getRequestPath(s);
  LOG_DBG("DAV", "LOCK %s (dummy)", path.c_str());

  if (isProtectedPath(path, WebDAVOperation::Lock)) {
    s.send(403, "text/plain", "Forbidden");
    return;
  }

  // Return a dummy lock token for client compatibility
  String xml =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<D:prop xmlns:D=\"DAV:\">\n"
      "<D:lockdiscovery><D:activelock>\n"
      "<D:locktype><D:write/></D:locktype>\n"
      "<D:lockscope><D:exclusive/></D:lockscope>\n"
      "<D:depth>infinity</D:depth>\n"
      "<D:owner><D:href>crosspoint</D:href></D:owner>\n"
      "<D:timeout>Second-3600</D:timeout>\n"
      "<D:locktoken><D:href>urn:uuid:dummy-lock-token</D:href></D:locktoken>\n"
      "<D:lockroot><D:href>/</D:href></D:lockroot>\n"
      "</D:activelock></D:lockdiscovery>\n"
      "</D:prop>\n";

  s.sendHeader("Lock-Token", "<urn:uuid:dummy-lock-token>");
  s.send(200, "application/xml; charset=\"utf-8\"", xml);
}

void WebDAVHandler::handleUnlock(WebServer& s) {
  String path = getRequestPath(s);
  LOG_DBG("DAV", "UNLOCK %s (dummy)", path.c_str());
  if (isProtectedPath(path, WebDAVOperation::Unlock)) {
    s.send(403, "text/plain", "Forbidden");
    return;
  }
  s.send(204);
}

// ── Utility functions ────────────────────────────────────────────────────────

String WebDAVHandler::getRequestPath(WebServer& s) const {
  String uri = s.uri();
  String decoded = WebServer::urlDecode(uri);

  // Normalize using FsHelpers
  std::string normalized = FsHelpers::normalisePath(decoded.c_str());
  String result = normalized.c_str();

  if (result.isEmpty()) return "/";
  if (!result.startsWith("/")) result = "/" + result;

  // Remove trailing slash unless root
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }

  return result;
}

String WebDAVHandler::getDestinationPath(WebServer& s) const {
  String dest = s.header("Destination");
  if (dest.isEmpty()) return "";

  // Extract path from full URL: http://host/path -> /path
  // Find the third slash (after http://)
  int schemeEnd = dest.indexOf("://");
  if (schemeEnd >= 0) {
    int pathStart = dest.indexOf('/', schemeEnd + 3);
    if (pathStart >= 0) {
      dest = dest.substring(pathStart);
    } else {
      dest = "/";
    }
  }

  String decoded = WebServer::urlDecode(dest);
  std::string normalized = FsHelpers::normalisePath(decoded.c_str());
  String result = normalized.c_str();

  if (result.isEmpty()) return "/";
  if (!result.startsWith("/")) result = "/" + result;

  // Remove trailing slash unless root
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }

  return result;
}

void WebDAVHandler::urlEncodePath(const String& path, String& out) const {
  out = "";
  for (unsigned int i = 0; i < path.length(); i++) {
    char c = path.charAt(i);
    if (c == '/') {
      out += '/';
    } else if (c == ' ') {
      out += "%20";
    } else if (c == '%') {
      out += "%25";
    } else if (c == '#') {
      out += "%23";
    } else if (c == '?') {
      out += "%3F";
    } else if (c == '&') {
      out += "%26";
    } else if ((uint8_t)c > 127) {
      // Encode non-ASCII bytes
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
      out += hex;
    } else {
      out += c;
    }
  }
}

bool WebDAVHandler::isProtectedPath(const String& path, const WebDAVOperation operation) const {
  return WebDAVPathPolicy::isProtected(path.c_str(), operation);
}

int WebDAVHandler::getDepth(WebServer& s) const {
  String depth = s.header("Depth");
  if (depth == "0") return 0;
  if (depth == "1") return 1;
  // "infinity" or missing → treat as 1 (Class 1 servers don't need to support infinity)
  return 1;
}

bool WebDAVHandler::getOverwrite(WebServer& s) const {
  String ow = s.header("Overwrite");
  if (ow == "F" || ow == "f") return false;
  return true;  // Default is T
}

String WebDAVHandler::getMimeType(const String& path) const {
  if (FsHelpers::hasEpubExtension(path)) return "application/epub+zip";
  if (FsHelpers::checkFileExtension(path, ".pdf")) return "application/pdf";
  if (FsHelpers::hasTxtExtension(path)) return "text/plain";
  if (FsHelpers::checkFileExtension(path, ".html") || FsHelpers::checkFileExtension(path, ".htm")) return "text/html";
  if (FsHelpers::checkFileExtension(path, ".css")) return "text/css";
  if (FsHelpers::checkFileExtension(path, ".js")) return "application/javascript";
  if (FsHelpers::checkFileExtension(path, ".json")) return "application/json";
  if (FsHelpers::checkFileExtension(path, ".xml")) return "application/xml";
  if (FsHelpers::hasJpgExtension(path)) return "image/jpeg";
  if (FsHelpers::hasPngExtension(path)) return "image/png";
  if (FsHelpers::hasGifExtension(path)) return "image/gif";
  if (FsHelpers::checkFileExtension(path, ".svg")) return "image/svg+xml";
  if (FsHelpers::checkFileExtension(path, ".zip")) return "application/zip";
  if (FsHelpers::checkFileExtension(path, ".gz")) return "application/gzip";
  return "application/octet-stream";
}
