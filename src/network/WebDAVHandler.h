#pragma once

#include <HalStorage.h>
#include <WebServer.h>

#include <array>

#include "WebDAVPathPolicy.h"

class WebDAVHandler : public RequestHandler {
 public:
  // RequestHandler interface
  bool canHandle(WebServer& server, HTTPMethod method, const String& uri) override;
  bool canRaw(WebServer& server, const String& uri) override;
  void raw(WebServer& server, const String& uri, HTTPRaw& raw) override;
  bool handle(WebServer& server, HTTPMethod method, const String& uri) override;

 private:
  // PUT streaming state (raw() is called in chunks)
  HalFile _putFile;
  String _putPath;
  bool _putOk = false;
  bool _putExisted = false;
  bool _putValidated = false;
  size_t _putBytes = 0;
  size_t _putBufferSize = 0;

  // The handler is heap-owned for the lifetime of the foreground web server.
  // WebDAV requests are handled serially, so GET and PUT can share one modest
  // buffer. This avoids a large request-stack frame and batches small network
  // chunks into fewer SD writes without allocating per request.
  static constexpr size_t TRANSFER_BUFFER_SIZE = 4096;
  std::array<uint8_t, TRANSFER_BUFFER_SIZE> _transferBuffer{};

  // WebDAV method handlers
  void handleOptions(WebServer& s);
  void handlePropfind(WebServer& s);
  void handleGet(WebServer& s);
  void handleHead(WebServer& s);
  void handlePut(WebServer& s);
  void handleDelete(WebServer& s);
  void handleMkcol(WebServer& s);
  void handleMove(WebServer& s);
  void handleCopy(WebServer& s);
  void handleLock(WebServer& s);
  void handleUnlock(WebServer& s);

  // Utilities
  String getRequestPath(WebServer& s) const;
  String getDestinationPath(WebServer& s) const;
  void urlEncodePath(const String& path, String& out) const;
  bool isProtectedPath(const String& path, WebDAVOperation operation) const;
  int getDepth(WebServer& s) const;
  bool getOverwrite(WebServer& s) const;
  bool flushPutBuffer();
  void sendPropEntry(WebServer& s, const String& href, bool isDir, size_t size, const String& lastModified) const;
  String getMimeType(const String& path) const;
  bool validateProgressSidecarFile(const String& targetPath, const String& candidatePath);
  void recoverProgressSidecar(const String& path);
};
