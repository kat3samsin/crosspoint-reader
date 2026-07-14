#include "ProgressXPathParser.h"

#include <cstring>
#include <limits>

namespace ProgressXPath {
namespace {

constexpr std::string_view DOCUMENT_PREFIX = "/body/DocFragment[";
constexpr std::string_view BODY_SUFFIX = "/body";
constexpr std::string_view TEXT_TERMINAL = "/text()";

bool isDigit(const char value) { return value >= '0' && value <= '9'; }

bool isNameStart(const char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool isNameCharacter(const char value) {
  return isNameStart(value) || isDigit(value) || value == '-' || value == ':';
}

bool parseUnsigned(const std::string_view value, size_t& position, const uint32_t maximum, uint32_t& parsed) {
  if (position >= value.size() || !isDigit(value[position])) {
    return false;
  }

  uint32_t result = 0;
  do {
    const uint32_t digit = static_cast<uint32_t>(value[position] - '0');
    if (result > (maximum - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
    position++;
  } while (position < value.size() && isDigit(value[position]));

  parsed = result;
  return true;
}

bool parseCharacterOffset(const std::string_view xpath, size_t& position, ParsedXPath& parsed) {
  if (position >= xpath.size() || xpath[position] != '.') {
    return false;
  }
  position++;

  uint32_t offset = 0;
  if (!parseUnsigned(xpath, position, static_cast<uint32_t>(std::numeric_limits<int32_t>::max()), offset) ||
      position != xpath.size()) {
    return false;
  }

  parsed.characterOffset = offset;
  parsed.hasCharacterOffset = true;
  return true;
}

bool parseTextTerminal(const std::string_view xpath, size_t& position, ParsedXPath& parsed) {
  position += TEXT_TERMINAL.size();

  uint32_t textNodeIndex = 1;
  if (position < xpath.size() && xpath[position] == '[') {
    position++;
    if (!parseUnsigned(xpath, position, std::numeric_limits<uint16_t>::max(), textNodeIndex) || textNodeIndex == 0 ||
        position >= xpath.size() || xpath[position] != ']') {
      return false;
    }
    position++;
    parsed.hasExplicitTextNodeIndex = true;
  }

  if (!parseCharacterOffset(xpath, position, parsed)) {
    return false;
  }
  parsed.textNodeIndex = static_cast<uint16_t>(textNodeIndex);
  parsed.hasTextTerminal = true;
  return true;
}

}  // namespace

bool parse(const std::string_view xpath, ParsedXPath& parsed) {
  parsed = {};
  if (xpath.empty() || xpath.size() > MAX_XPATH_BYTES || !xpath.starts_with(DOCUMENT_PREFIX)) {
    return false;
  }

  size_t position = DOCUMENT_PREFIX.size();
  uint32_t documentFragment = 0;
  if (!parseUnsigned(xpath, position, std::numeric_limits<uint16_t>::max(), documentFragment) ||
      documentFragment == 0 || position >= xpath.size() || xpath[position] != ']') {
    return false;
  }
  parsed.documentFragment = static_cast<uint16_t>(documentFragment);
  position++;

  // Some KOReader servers use the fragment itself (optionally with .0) as the
  // chapter-start position instead of spelling out /body.
  if (position == xpath.size()) {
    return true;
  }
  if (xpath[position] == '.') {
    return parseCharacterOffset(xpath, position, parsed) && parsed.characterOffset == 0;
  }

  if (xpath.compare(position, BODY_SUFFIX.size(), BODY_SUFFIX) != 0) {
    return false;
  }
  position += BODY_SUFFIX.size();
  if (position == xpath.size()) {
    return true;
  }
  if (xpath[position] != '/') {
    return false;
  }
  if (position + 1 == xpath.size()) {
    return true;
  }

  while (position < xpath.size()) {
    if (xpath.compare(position, TEXT_TERMINAL.size(), TEXT_TERMINAL) == 0) {
      return parsed.stepCount > 0 && parseTextTerminal(xpath, position, parsed);
    }

    if (xpath[position] != '/' || parsed.stepCount >= MAX_DEPTH) {
      return false;
    }
    position++;
    if (position >= xpath.size() || !isNameStart(xpath[position])) {
      return false;
    }

    const size_t tagStart = position;
    while (position < xpath.size() && isNameCharacter(xpath[position])) {
      position++;
    }
    const size_t tagLength = position - tagStart;
    if (tagLength == 0 || tagLength >= MAX_TAG_BYTES) {
      return false;
    }

    uint32_t siblingIndex = 1;
    if (position < xpath.size() && xpath[position] == '[') {
      position++;
      if (!parseUnsigned(xpath, position, std::numeric_limits<uint16_t>::max(), siblingIndex) ||
          position >= xpath.size() || xpath[position] != ']') {
        return false;
      }
      position++;
      // Readest treats element [0] as the first child for legacy XPointer compatibility.
      if (siblingIndex == 0) {
        siblingIndex = 1;
      }
    }

    Step& step = parsed.steps[parsed.stepCount];
    memcpy(step.tag, xpath.data() + tagStart, tagLength);
    step.tag[tagLength] = '\0';
    step.siblingIndex = static_cast<uint16_t>(siblingIndex);
    parsed.stepCount++;

    if (position == xpath.size()) {
      return true;
    }
    if (xpath[position] == '.') {
      return parseCharacterOffset(xpath, position, parsed);
    }
    if (xpath[position] != '/') {
      return false;
    }
  }

  return false;
}

}  // namespace ProgressXPath
