#include <gtest/gtest.h>

#include "src/network/WebDAVPathPolicy.h"

namespace {

using WebDAVPathPolicy::isProtected;

TEST(WebDAVPathPolicyTest, AllowsOnlyReadWriteOperationsForReadestManifest) {
  constexpr auto path = WebDAVPathPolicy::READEST_LIBRARY_MANIFEST;

  EXPECT_FALSE(isProtected(path, WebDAVOperation::Get));
  EXPECT_FALSE(isProtected(path, WebDAVOperation::Head));
  EXPECT_FALSE(isProtected(path, WebDAVOperation::Put));

  EXPECT_TRUE(isProtected(path, WebDAVOperation::Propfind));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Delete));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Mkcol));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Move));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Copy));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Lock));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Unlock));
}

TEST(WebDAVPathPolicyTest, AllowsReadestToWriteOnlyItsExactProgressSidecar) {
  constexpr auto path =
      "/.crosspoint/readest-sync/0123456789abcdef0123456789abcdef.readest.json";

  EXPECT_FALSE(isProtected(path, WebDAVOperation::Get));
  EXPECT_FALSE(isProtected(path, WebDAVOperation::Head));
  EXPECT_FALSE(isProtected(path, WebDAVOperation::Put));
  EXPECT_EQ(WebDAVPathPolicy::classifyProgressSidecar(path),
            WebDAVPathPolicy::ProgressSidecarKind::ReadestOwned);

  EXPECT_TRUE(isProtected(path, WebDAVOperation::Propfind));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Delete));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Mkcol));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Move));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Copy));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Lock));
  EXPECT_TRUE(isProtected(path, WebDAVOperation::Unlock));
}

TEST(WebDAVPathPolicyTest, MakesCrossPointProgressSidecarsReadOnly) {
  constexpr auto path =
      "/.crosspoint/readest-sync/0123456789abcdef0123456789abcdef.crosspoint.json";

  EXPECT_FALSE(isProtected(path, WebDAVOperation::Get));
  EXPECT_FALSE(isProtected(path, WebDAVOperation::Head));
  EXPECT_EQ(WebDAVPathPolicy::classifyProgressSidecar(path),
            WebDAVPathPolicy::ProgressSidecarKind::CrossPointOwned);

  for (const auto operation : {WebDAVOperation::Propfind, WebDAVOperation::Put,
                               WebDAVOperation::Delete, WebDAVOperation::Mkcol,
                               WebDAVOperation::Move, WebDAVOperation::Copy,
                               WebDAVOperation::Lock, WebDAVOperation::Unlock}) {
    EXPECT_TRUE(isProtected(path, operation));
  }
}

TEST(WebDAVPathPolicyTest, RejectsNearMissProgressSidecarPaths) {
  for (const auto path : {
           "/.crosspoint/readest-sync/0123456789ABCDEF0123456789abcdef.readest.json",
           "/.crosspoint/readest-sync/short.readest.json",
           "/.crosspoint/readest-sync/0123456789abcdef0123456789abcdef.readest.json/child",
           "/.crosspoint/readest-sync/0123456789abcdef0123456789abcdef.crosspoint.json.bak",
           "/.crosspoint/readest-sync/nested/0123456789abcdef0123456789abcdef.readest.json",
       }) {
    EXPECT_EQ(WebDAVPathPolicy::classifyProgressSidecar(path),
              WebDAVPathPolicy::ProgressSidecarKind::None)
        << path;
    EXPECT_TRUE(isProtected(path, WebDAVOperation::Get)) << path;
    EXPECT_TRUE(isProtected(path, WebDAVOperation::Put)) << path;
  }
}

TEST(WebDAVPathPolicyTest, ProtectsEveryOtherDotPrefixedSegment) {
  for (const auto path : {
           "/.crosspoint",
           "/.crosspoint/settings.json",
           "/.crosspoint/readest-library.json.bak",
           "/.crosspoint/readest-library.json/child",
           "/books/.private/book.epub",
           "/.CrossPoint/readest-library.json",
       }) {
    EXPECT_TRUE(isProtected(path, WebDAVOperation::Get)) << path;
    EXPECT_TRUE(isProtected(path, WebDAVOperation::Head)) << path;
    EXPECT_TRUE(isProtected(path, WebDAVOperation::Put)) << path;
  }
}

TEST(WebDAVPathPolicyTest, ProtectsReservedSystemDirectoriesAtAnyDepth) {
  for (const auto path : {
           "/System Volume Information",
           "/System Volume Information/indexer.dat",
           "/library/XTCache/page.bin",
       }) {
    EXPECT_TRUE(isProtected(path, WebDAVOperation::Get)) << path;
    EXPECT_TRUE(isProtected(path, WebDAVOperation::Put)) << path;
  }
}

TEST(WebDAVPathPolicyTest, KeepsPublicBookPathsAvailable) {
  for (const auto path : {
           "/",
           "/Witchcraft for Wayward Girls.epub",
           "/Fiction/A Short Stay in Hell.epub",
           "/XTCache Notes.epub",
       }) {
    EXPECT_FALSE(isProtected(path, WebDAVOperation::Get)) << path;
    EXPECT_FALSE(isProtected(path, WebDAVOperation::Put)) << path;
    EXPECT_FALSE(isProtected(path, WebDAVOperation::Delete)) << path;
  }
}

}  // namespace
