#pragma once

#include <string>

namespace AtomicFileReplace {

template <typename FileSystem>
bool replace(const std::string& temporaryPath, const std::string& finalPath,
             const std::string& backupPath, FileSystem& fileSystem) {
  if (!fileSystem.exists(temporaryPath)) return false;
  fileSystem.remove(backupPath);
  const bool hadFinal = fileSystem.exists(finalPath);
  if (hadFinal && !fileSystem.rename(finalPath, backupPath)) return false;

  if (fileSystem.rename(temporaryPath, finalPath)) {
    if (hadFinal) fileSystem.remove(backupPath);
    return true;
  }

  if (hadFinal) fileSystem.rename(backupPath, finalPath);
  return false;
}

template <typename FileSystem, typename Validator>
std::string recover(const std::string& finalPath, const std::string& backupPath,
                    const std::string& temporaryPath, FileSystem& fileSystem,
                    Validator&& validator) {
  if (fileSystem.exists(finalPath) && validator(finalPath)) return finalPath;

  for (const std::string* candidate : {&backupPath, &temporaryPath}) {
    if (!fileSystem.exists(*candidate) || !validator(*candidate)) continue;
    if (fileSystem.exists(finalPath)) fileSystem.remove(finalPath);
    if (fileSystem.rename(*candidate, finalPath)) return finalPath;
    return *candidate;
  }
  return {};
}

}  // namespace AtomicFileReplace
