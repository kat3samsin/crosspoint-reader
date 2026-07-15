#pragma once

#include <HighlightRange.h>

#include <string>
#include <vector>

// Persists source-word ranges for in-book rendering and appends a readable
// clipping to markdown. Depending on SETTINGS.highlightFileMode, the passage
// goes to /Highlights/<book>.md or /Highlights.md.
namespace HighlightStore {

bool save(const std::string& bookPath, const std::string& bookTitle, const std::string& chapterTitle,
          const std::string& passage, const Highlights::Range& range);
bool loadRanges(const std::string& bookPath, std::vector<Highlights::Range>& ranges);

}  // namespace HighlightStore
