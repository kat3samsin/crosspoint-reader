#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Highlights {

inline constexpr uint32_t NO_WORD_ORDINAL = UINT32_MAX;
inline constexpr size_t MAX_RANGES_PER_BOOK = 256;
inline constexpr size_t MAX_RANGE_FILE_BYTES = 16 * 1024;

struct Range {
  uint16_t spineIndex = 0;
  uint32_t startWord = NO_WORD_ORDINAL;
  uint32_t endWord = NO_WORD_ORDINAL;
};

bool isValid(const Range& range);
bool contains(const Range& range, uint16_t spineIndex, uint32_t wordOrdinal);
bool parseRanges(std::string_view data, std::vector<Range>& ranges);
bool serializeRanges(const std::vector<Range>& ranges, std::string& data);

}  // namespace Highlights
