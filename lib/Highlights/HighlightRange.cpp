#include "HighlightRange.h"

#include <algorithm>
#include <limits>

namespace Highlights {
namespace {

constexpr std::string_view HEADER = "crosspoint-highlights-v1\n";

bool parseUnsigned(std::string_view line, size_t& position, const uint32_t maximum, uint32_t& value) {
  if (position >= line.size() || line[position] < '0' || line[position] > '9') return false;
  uint32_t parsed = 0;
  while (position < line.size() && line[position] >= '0' && line[position] <= '9') {
    const uint32_t digit = static_cast<uint32_t>(line[position] - '0');
    if (parsed > (maximum - digit) / 10) return false;
    parsed = parsed * 10 + digit;
    position++;
  }
  value = parsed;
  return true;
}

bool parseLine(const std::string_view line, Range& range) {
  size_t position = 0;
  uint32_t spine = 0;
  if (!parseUnsigned(line, position, std::numeric_limits<uint16_t>::max(), spine) || position >= line.size() ||
      line[position++] != ' ' || !parseUnsigned(line, position, NO_WORD_ORDINAL - 1, range.startWord) ||
      position >= line.size() || line[position++] != ' ' ||
      !parseUnsigned(line, position, NO_WORD_ORDINAL - 1, range.endWord) || position != line.size()) {
    return false;
  }
  range.spineIndex = static_cast<uint16_t>(spine);
  return isValid(range);
}

}  // namespace

bool isValid(const Range& range) {
  return range.startWord != NO_WORD_ORDINAL && range.endWord != NO_WORD_ORDINAL && range.startWord <= range.endWord;
}

bool contains(const Range& range, const uint16_t spineIndex, const uint32_t wordOrdinal) {
  return isValid(range) && range.spineIndex == spineIndex && wordOrdinal >= range.startWord &&
         wordOrdinal <= range.endWord;
}

bool parseRanges(const std::string_view data, std::vector<Range>& ranges) {
  ranges.clear();
  if (data.empty()) return true;
  if (data.size() > MAX_RANGE_FILE_BYTES || !data.starts_with(HEADER)) return false;

  size_t position = HEADER.size();
  while (position < data.size()) {
    const size_t end = data.find('\n', position);
    if (end == std::string_view::npos || end == position || ranges.size() >= MAX_RANGES_PER_BOOK) return false;
    Range range;
    if (!parseLine(data.substr(position, end - position), range)) return false;
    ranges.push_back(range);
    position = end + 1;
  }
  return true;
}

bool serializeRanges(const std::vector<Range>& ranges, std::string& data) {
  if (ranges.size() > MAX_RANGES_PER_BOOK ||
      !std::all_of(ranges.begin(), ranges.end(), [](const Range& range) { return isValid(range); })) {
    return false;
  }

  data.assign(HEADER.data(), HEADER.size());
  for (const auto& range : ranges) {
    data += std::to_string(range.spineIndex);
    data.push_back(' ');
    data += std::to_string(range.startWord);
    data.push_back(' ');
    data += std::to_string(range.endWord);
    data.push_back('\n');
  }
  return data.size() <= MAX_RANGE_FILE_BYTES;
}

}  // namespace Highlights
