#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>

#include "ReadestProgress/AtomicFileReplace.h"
#include "ReadestProgress/ReadestLibraryOwnership.h"
#include "ReadestProgress/ReadestProgressSidecar.h"

namespace {

constexpr char DOCUMENT[] = "0123456789abcdef0123456789abcdef";
constexpr char READEST_REVISION[] = "11111111111111111111111111111111";
constexpr char CROSSPOINT_REVISION[] = "b3c9f417197abd2c175e33cac4e6d24b";
constexpr char XPOINTER[] = "/body/DocFragment[2]/body/p[3]/text().4";

ReadestProgress::CrossPointSidecar makeCrossPointSidecar() {
  return ReadestProgress::createCrossPointSidecar(DOCUMENT, CROSSPOINT_REVISION, XPOINTER, 0.25f,
                                                   READEST_REVISION, {1, 2, 8});
}

TEST(ReadestProgressRevision, BuildsTheDocumentedDomainAndBigEndianFloatPreimage) {
  std::string input;
  ASSERT_TRUE(ReadestProgress::buildCrossPointRevisionInput(DOCUMENT, XPOINTER, 0.25f, input));
  std::string expected = "crosspoint-progress-v2";
  expected.push_back('\0');
  expected += DOCUMENT;
  expected.push_back('\0');
  expected += XPOINTER;
  expected.push_back('\0');
  expected.append("\x3e\x80\x00\x00", 4);
  EXPECT_EQ(input, expected);
}

TEST(ReadestProgressRevision, ExcludesCoordinatesAndAcknowledgement) {
  const auto first = ReadestProgress::createCrossPointSidecar(DOCUMENT, CROSSPOINT_REVISION, XPOINTER, 0.25f,
                                                               READEST_REVISION, {1, 2, 8});
  const auto second = ReadestProgress::createCrossPointSidecar(DOCUMENT, CROSSPOINT_REVISION, XPOINTER, 0.25f, "",
                                                                {7, 99, 100});
  EXPECT_EQ(first.revision, second.revision);
}

TEST(ReadestProgressSidecar, ParsesStrictReadestSchema) {
  const std::string json =
      R"({"schemaVersion":2,"document":"0123456789abcdef0123456789abcdef","revision":"11111111111111111111111111111111","xpointer":"/body/DocFragment[2]/body/p[3]/text().4","percentage":0.25,"basedOnCrosspoint":"22222222222222222222222222222222"})";
  ReadestProgress::ReadestSidecar sidecar;
  EXPECT_TRUE(ReadestProgress::parseReadestSidecar(json, sidecar));
  EXPECT_EQ(sidecar.document, DOCUMENT);
  EXPECT_EQ(sidecar.revision, READEST_REVISION);
  EXPECT_EQ(sidecar.xpointer, XPOINTER);
  EXPECT_FLOAT_EQ(sidecar.percentage, 0.25f);
  EXPECT_EQ(sidecar.basedOnCrosspoint, "22222222222222222222222222222222");
}

TEST(ReadestProgressSidecar, AcceptsIntegerBoundaryPercentagesFromJsonStringify) {
  for (const char* percentage : {"0", "1"}) {
    const std::string json =
        std::string(R"({"schemaVersion":2,"document":")") + DOCUMENT + R"(","revision":")" +
        READEST_REVISION + R"(","xpointer":")" + XPOINTER + R"(","percentage":)" + percentage +
        R"(,"basedOnCrosspoint":null})";
    ReadestProgress::ReadestSidecar sidecar;
    ASSERT_TRUE(ReadestProgress::parseReadestSidecar(json, sidecar)) << percentage;
    EXPECT_FLOAT_EQ(sidecar.percentage, percentage[0] == '0' ? 0.0f : 1.0f);
  }
}

TEST(ReadestProgressSidecar, RoundTripsCrossPointSchemaAndVerifiesRevision) {
  const auto sidecar = makeCrossPointSidecar();
  std::string json;
  ASSERT_TRUE(ReadestProgress::serializeCrossPointSidecar(sidecar, json));
  EXPECT_LE(json.size(), ReadestProgress::MAX_SIDECAR_BYTES);

  ReadestProgress::CrossPointSidecar parsed;
  ASSERT_TRUE(ReadestProgress::parseCrossPointSidecar(json, parsed));
  EXPECT_TRUE(ReadestProgress::matchesCrossPointRevision(parsed, CROSSPOINT_REVISION));
  EXPECT_EQ(parsed.document, sidecar.document);
  EXPECT_EQ(parsed.revision, sidecar.revision);
  EXPECT_EQ(parsed.xpointer, sidecar.xpointer);
  EXPECT_FLOAT_EQ(parsed.percentage, sidecar.percentage);
  EXPECT_EQ(parsed.appliedReadest, sidecar.appliedReadest);
  EXPECT_EQ(parsed.position.spineIndex, 1);
  EXPECT_EQ(parsed.position.pageNumber, 2);
  EXPECT_EQ(parsed.position.pageCount, 8);
}

TEST(ReadestProgressSidecar, RetainsExactPercentageBitsAcrossCrossPointJson) {
  auto sidecar = makeCrossPointSidecar();
  sidecar.percentage = 0.12345679f;
  std::string json;
  ASSERT_TRUE(ReadestProgress::serializeCrossPointSidecar(sidecar, json));

  ReadestProgress::CrossPointSidecar parsed;
  ASSERT_TRUE(ReadestProgress::parseCrossPointSidecar(json, parsed));
  EXPECT_EQ(std::bit_cast<std::uint32_t>(parsed.percentage),
            std::bit_cast<std::uint32_t>(sidecar.percentage));
  EXPECT_TRUE(ReadestProgress::matchesCrossPointRevision(parsed, CROSSPOINT_REVISION));
}

TEST(ReadestProgressSidecar, RejectsOversizeMalformedAndNonFinitePayloads) {
  ReadestProgress::ReadestSidecar readest;
  EXPECT_FALSE(ReadestProgress::parseReadestSidecar(std::string(ReadestProgress::MAX_SIDECAR_BYTES + 1, 'x'), readest));

  for (const char* percentage : {"-0.1", "1.1", "1e999", "NaN", "\"0.5\""}) {
    const std::string json =
        std::string(R"({"schemaVersion":2,"document":")") + DOCUMENT + R"(","revision":")" +
        READEST_REVISION + R"(","xpointer":")" + XPOINTER + R"(","percentage":)" + percentage +
        R"(,"basedOnCrosspoint":""})";
    EXPECT_FALSE(ReadestProgress::parseReadestSidecar(json, readest)) << percentage;
  }
}

TEST(ReadestProgressSidecar, RejectsWrongSchemaAndIgnoresUnknownFields) {
  ReadestProgress::ReadestSidecar sidecar;
  const std::string prefix =
      std::string(R"({"schemaVersion":2,"document":")") + DOCUMENT + R"(","revision":")" +
      READEST_REVISION + R"(","xpointer":")" + XPOINTER + R"(","percentage":0.25,"basedOnCrosspoint":"")";
  EXPECT_TRUE(ReadestProgress::parseReadestSidecar(prefix + R"(,"futureField":true})", sidecar));

  std::string wrongVersion = prefix + "}";
  wrongVersion.replace(wrongVersion.find("schemaVersion\":2"), 15, "schemaVersion\":1");
  EXPECT_FALSE(ReadestProgress::parseReadestSidecar(wrongVersion, sidecar));
}

TEST(ReadestProgressSidecar, UsesJsonNullForAbsentOptionalRevisions) {
  const std::string readestJson =
      std::string(R"({"schemaVersion":2,"document":")") + DOCUMENT + R"(","revision":")" +
      READEST_REVISION + R"(","xpointer":")" + XPOINTER +
      R"(","percentage":0.25,"basedOnCrosspoint":null,"future":42})";
  ReadestProgress::ReadestSidecar readest;
  ASSERT_TRUE(ReadestProgress::parseReadestSidecar(readestJson, readest));
  EXPECT_TRUE(readest.basedOnCrosspoint.empty());
  std::string serialized;
  ASSERT_TRUE(ReadestProgress::serializeReadestSidecar(readest, serialized));
  EXPECT_NE(serialized.find("\"basedOnCrosspoint\":null"), std::string::npos);

  auto crossPoint = makeCrossPointSidecar();
  crossPoint.appliedReadest.clear();
  ASSERT_TRUE(ReadestProgress::serializeCrossPointSidecar(crossPoint, serialized));
  EXPECT_NE(serialized.find("\"appliedReadest\":null"), std::string::npos);
  ReadestProgress::CrossPointSidecar parsed;
  ASSERT_TRUE(ReadestProgress::parseCrossPointSidecar(serialized, parsed));
  EXPECT_TRUE(parsed.appliedReadest.empty());
}

TEST(ReadestProgressSidecar, RejectsInvalidXPointerIdentifiersAndTuple) {
  auto sidecar = makeCrossPointSidecar();
  std::string json;

  sidecar.document[0] = 'A';
  EXPECT_FALSE(ReadestProgress::serializeCrossPointSidecar(sidecar, json));
  sidecar = makeCrossPointSidecar();
  sidecar.xpointer = "/not/a/readest/xpointer";
  EXPECT_FALSE(ReadestProgress::serializeCrossPointSidecar(sidecar, json));
  sidecar = makeCrossPointSidecar();
  sidecar.position = {1, 8, 8};
  EXPECT_FALSE(ReadestProgress::serializeCrossPointSidecar(sidecar, json));
}

TEST(ReadestProgressSidecar, RejectsTamperedCrossPointRevision) {
  auto sidecar = makeCrossPointSidecar();
  std::string json;
  ASSERT_TRUE(ReadestProgress::serializeCrossPointSidecar(sidecar, json));
  json.replace(json.find(sidecar.revision), sidecar.revision.size(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  ReadestProgress::CrossPointSidecar parsed;
  ASSERT_TRUE(ReadestProgress::parseCrossPointSidecar(json, parsed));
  EXPECT_FALSE(ReadestProgress::matchesCrossPointRevision(parsed, sidecar.revision));
}

TEST(ReadestProgressLifecycle, PreservesAcknowledgementOnlyAtTheSameSavedTuple) {
  const auto previous = makeCrossPointSidecar();
  EXPECT_EQ(ReadestProgress::appliedRevisionForExit(&previous, {1, 2, 8}), READEST_REVISION);
  EXPECT_TRUE(ReadestProgress::appliedRevisionForExit(&previous, {1, 3, 8}).empty());
  EXPECT_TRUE(ReadestProgress::appliedRevisionForExit(nullptr, {1, 2, 8}).empty());
}

TEST(ReadestProgressPaths, UseOnlyExactLowercaseDocumentIdentifiers) {
  EXPECT_EQ(ReadestProgress::readestSidecarPath(DOCUMENT),
            "/.crosspoint/readest-sync/0123456789abcdef0123456789abcdef.readest.json");
  EXPECT_EQ(ReadestProgress::crossPointSidecarPath(DOCUMENT),
            "/.crosspoint/readest-sync/0123456789abcdef0123456789abcdef.crosspoint.json");
  EXPECT_TRUE(ReadestProgress::readestSidecarPath("BAD").empty());
}

struct FakeFileSystem {
  std::map<std::string, std::string> files;
  std::set<std::string> failingRenames;

  bool exists(const std::string& path) const { return files.contains(path); }
  bool remove(const std::string& path) { return files.erase(path) > 0; }
  bool rename(const std::string& from, const std::string& to) {
    if (failingRenames.contains(from + "->" + to) || !exists(from) || exists(to)) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
};

TEST(ReadestProgressAtomicFile, RestoresTheLastValidFileWhenReplacementFails) {
  FakeFileSystem fileSystem{{{"final", "old"}, {"temp", "new"}}, {"temp->final"}};
  EXPECT_FALSE(AtomicFileReplace::replace("temp", "final", "backup", fileSystem));
  EXPECT_EQ(fileSystem.files["final"], "old");
  EXPECT_EQ(fileSystem.files["temp"], "new");
}

TEST(ReadestProgressAtomicFile, WebDavReadCanValidateAndRecoverBackupAfterFailedRollback) {
  FakeFileSystem fileSystem{{{"progress.readest.json", "old-valid"},
                              {"progress.readest.json.davtmp", "new-valid"}},
                             {"progress.readest.json.davtmp->progress.readest.json",
                              "progress.readest.json.davbak->progress.readest.json"}};
  EXPECT_FALSE(AtomicFileReplace::replace("progress.readest.json.davtmp", "progress.readest.json",
                                         "progress.readest.json.davbak", fileSystem));
  EXPECT_FALSE(fileSystem.exists("progress.readest.json"));
  EXPECT_EQ(fileSystem.files["progress.readest.json.davbak"], "old-valid");

  fileSystem.failingRenames.clear();
  auto validSidecar = [&](const std::string& path) { return fileSystem.files[path] == "old-valid"; };
  EXPECT_EQ(AtomicFileReplace::recover("progress.readest.json", "progress.readest.json.davbak",
                                      "progress.readest.json.davtmp", fileSystem, validSidecar),
            "progress.readest.json");
  EXPECT_EQ(fileSystem.files["progress.readest.json"], "old-valid");
}

TEST(ReadestProgressAtomicFile, RecoversValidatedBackupOrTemporaryFiles) {
  FakeFileSystem fromBackup{{{"backup", "valid"}, {"temp", "partial"}}, {}};
  auto validator = [](const std::string& path) { return path != "temp"; };
  EXPECT_EQ(AtomicFileReplace::recover("final", "backup", "temp", fromBackup, validator), "final");
  EXPECT_EQ(fromBackup.files["final"], "valid");

  FakeFileSystem fromTemp{{{"temp", "valid"}}, {}};
  auto valid = [](const std::string&) { return true; };
  EXPECT_EQ(AtomicFileReplace::recover("final", "backup", "temp", fromTemp, valid), "final");
  EXPECT_EQ(fromTemp.files["final"], "valid");
}

TEST(ReadestProgressAtomicFile, ReplacesACorruptFinalWithTheLastValidBackup) {
  FakeFileSystem fileSystem{{{"final", "corrupt"}, {"backup", "valid"}, {"temp", "partial"}}, {}};
  auto validator = [&](const std::string& path) { return fileSystem.files[path] == "valid"; };
  EXPECT_EQ(AtomicFileReplace::recover("final", "backup", "temp", fileSystem, validator), "final");
  EXPECT_EQ(fileSystem.files["final"], "valid");
  EXPECT_FALSE(fileSystem.exists("backup"));
}

TEST(ReadestLibraryOwnership, MatchesOnlyTheExactManifestOwnedRootBook) {
  constexpr std::string_view manifest =
      R"({"version":1,"books":{"first":{"path":"/Witchcraft for Wayward Girls.epub","size":10,"revision":1,"state":"active"},"second":{"path":"/Other.epub","size":20,"revision":1,"state":"uploading"}}})";
  EXPECT_TRUE(ReadestProgress::manifestOwnsRootBook(manifest, "/Witchcraft for Wayward Girls.epub"));
  EXPECT_TRUE(ReadestProgress::manifestOwnsRootBook(manifest, "/Other.epub"));
  EXPECT_FALSE(ReadestProgress::manifestOwnsRootBook(manifest, "/Witchcraft for Wayward Girl.epub"));
  EXPECT_FALSE(ReadestProgress::manifestOwnsRootBook(manifest, "/read/Witchcraft for Wayward Girls.epub"));
}

}  // namespace
