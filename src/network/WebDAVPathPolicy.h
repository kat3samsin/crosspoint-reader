#pragma once

#include <cstdint>
#include <string_view>

enum class WebDAVOperation : std::uint8_t {
  Propfind,
  Get,
  Head,
  Put,
  Delete,
  Mkcol,
  Move,
  Copy,
  Lock,
  Unlock,
};

namespace WebDAVPathPolicy {

inline constexpr std::string_view READEST_LIBRARY_MANIFEST =
    "/.crosspoint/readest-library.json";
inline constexpr std::string_view READEST_PROGRESS_DIRECTORY = "/.crosspoint/readest-sync";
inline constexpr std::size_t MAX_READEST_PROGRESS_SIDECAR_BYTES = 2048;

enum class ProgressSidecarKind : std::uint8_t {
  None,
  ReadestOwned,
  CrossPointOwned,
};

ProgressSidecarKind classifyProgressSidecar(std::string_view path);
bool isProtected(std::string_view path, WebDAVOperation operation);

}  // namespace WebDAVPathPolicy
