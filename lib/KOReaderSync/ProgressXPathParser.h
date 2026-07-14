#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ProgressXPath {

inline constexpr size_t MAX_DEPTH = 16;
inline constexpr size_t MAX_TAG_BYTES = 12;
inline constexpr size_t MAX_XPATH_BYTES = 1024;

struct Step {
  char tag[MAX_TAG_BYTES]{};
  uint16_t siblingIndex = 1;
};

struct ParsedXPath {
  Step steps[MAX_DEPTH]{};
  uint32_t characterOffset = 0;
  uint16_t documentFragment = 0;
  uint16_t textNodeIndex = 1;
  uint8_t stepCount = 0;
  bool hasCharacterOffset = false;
  bool hasTextTerminal = false;
  // false for /text().N (cumulative descendant text), true for /text()[K].N.
  bool hasExplicitTextNodeIndex = false;
};

static_assert(sizeof(ParsedXPath) <= 256, "Parsed XPath must stay within the reader stack budget");

// Parse a complete KOReader/Readest XPointer. On success, documentFragment and
// all indexes are normalized to one-based values. Explicit element index zero
// is accepted as a legacy alias for the first sibling because Readest emits and
// accepts that form. The caller must ignore parsed when false is returned.
bool parse(std::string_view xpath, ParsedXPath& parsed);

}  // namespace ProgressXPath
