#include "PerformanceBenchmark.h"

#ifdef ENABLE_PERF_BENCHMARK

#include <Arduino.h>
#include <Logging.h>

namespace PerformanceBenchmark {
namespace {

uint32_t bookOpenStartedAtUs = 0;
uint32_t pageTurnStartedAtUs = 0;
uint32_t bookIteration = 0;
uint32_t pageTurnIteration = 0;
bool bookOpenPending = false;
bool pageTurnPending = false;
bool pageTurnOverlapped = false;
bool epubLoadPending = false;
bool bookCacheKnown = false;
bool bookCacheHit = false;
bool pageTurnForward = true;
uint32_t pageTurnSpineIndex = 0;
uint32_t pageTurnFromPage = 0;
uint32_t pageTurnToPage = 0;
uint8_t pageTurnFontSize = 0;
bool pageTurnTextAntialiasing = false;
PageRefreshMode pageTurnRefreshMode = PageRefreshMode::UNKNOWN;
bool bootToHomePending = false;
uint32_t epubLoadDurationUs = 0;
uint32_t epubLoadHeapFreeBytes = 0;

uint32_t elapsedUs(const uint32_t startedAtUs, const uint32_t finishedAtUs) {
  // Unsigned subtraction remains correct when micros() wraps once.
  return finishedAtUs - startedAtUs;
}

const char* cacheName(const bool known, const bool hit) {
  if (!known) return "unknown";
  return hit ? "hit" : "miss";
}

const char* refreshModeName(const PageRefreshMode refreshMode) {
  switch (refreshMode) {
    case PageRefreshMode::FAST:
      return "fast";
    case PageRefreshMode::HALF:
      return "half";
    case PageRefreshMode::IMAGE:
      return "image";
    case PageRefreshMode::UNKNOWN:
    default:
      return "unknown";
  }
}

}  // namespace

uint32_t nowUs() { return micros(); }

void expectBootToHome() { bootToHomePending = true; }

void recordHomePaint() {
  if (!bootToHomePending) return;
  const uint32_t durationUs = nowUs();
  const uint32_t heapFreeBytes = ESP.getFreeHeap();
  bootToHomePending = false;
  logSerial.printf(
      "PERF {\"v\":2,\"scenario\":\"boot_to_home\",\"iteration\":1,\"duration_us\":%lu,"
      "\"heap_free_bytes\":%lu}\n",
      static_cast<unsigned long>(durationUs), static_cast<unsigned long>(heapFreeBytes));
}

void beginBookOpen() {
  bookIteration++;
  bookOpenStartedAtUs = nowUs();
  bookOpenPending = true;
  bookCacheKnown = false;
  epubLoadPending = false;
}

void setBookCacheHit(const bool cacheHit) {
  bookCacheKnown = true;
  bookCacheHit = cacheHit;
}

void recordEpubLoad(const uint32_t startedAtUs, const bool cacheHit) {
  epubLoadDurationUs = elapsedUs(startedAtUs, nowUs());
  epubLoadHeapFreeBytes = ESP.getFreeHeap();
  bookCacheKnown = true;
  bookCacheHit = cacheHit;
  epubLoadPending = true;
}

void finishBookOpen() {
  if (!bookOpenPending) return;
  const uint32_t durationUs = elapsedUs(bookOpenStartedAtUs, nowUs());
  const uint32_t heapFreeBytes = ESP.getFreeHeap();
  bookOpenPending = false;
  logSerial.printf(
      "PERF {\"v\":2,\"scenario\":\"book_open\",\"iteration\":%lu,\"duration_us\":%lu,"
      "\"epub_index_cache\":\"%s\",\"managed\":false,\"heap_free_bytes\":%lu}\n",
      static_cast<unsigned long>(bookIteration), static_cast<unsigned long>(durationUs),
      cacheName(bookCacheKnown, bookCacheHit), static_cast<unsigned long>(heapFreeBytes));
  // Flush the nested phase record only after sampling the outer duration so
  // its serial write cannot inflate book_open.
  if (epubLoadPending) {
    logSerial.printf(
        "PERF {\"v\":2,\"scenario\":\"epub_load\",\"iteration\":%lu,\"duration_us\":%lu,"
        "\"epub_index_cache\":\"%s\",\"heap_free_bytes\":%lu}\n",
        static_cast<unsigned long>(bookIteration), static_cast<unsigned long>(epubLoadDurationUs),
        cacheName(bookCacheKnown, bookCacheHit), static_cast<unsigned long>(epubLoadHeapFreeBytes));
    epubLoadPending = false;
  }
}

void beginPageTurn(const bool forward, const uint32_t spineIndex, const uint32_t fromPage,
                   const uint32_t toPage, const uint8_t fontSize, const bool textAntialiasing) {
  if (pageTurnPending) {
    // Multiple inputs before the panel paint completes are coalesced into one
    // render. Discard that ambiguous sample instead of attributing it to the
    // latest input.
    pageTurnOverlapped = true;
    return;
  }
  pageTurnIteration++;
  pageTurnStartedAtUs = nowUs();
  pageTurnForward = forward;
  pageTurnSpineIndex = spineIndex;
  pageTurnFromPage = fromPage;
  pageTurnToPage = toPage;
  pageTurnFontSize = fontSize;
  pageTurnTextAntialiasing = textAntialiasing;
  pageTurnRefreshMode = PageRefreshMode::UNKNOWN;
  pageTurnPending = true;
  pageTurnOverlapped = false;
}

void setPageTurnRefreshMode(const PageRefreshMode refreshMode) {
  if (pageTurnPending) pageTurnRefreshMode = refreshMode;
}

void finishPageTurn() {
  if (!pageTurnPending) return;
  if (pageTurnOverlapped) {
    pageTurnPending = false;
    pageTurnOverlapped = false;
    return;
  }
  const uint32_t durationUs = elapsedUs(pageTurnStartedAtUs, nowUs());
  const uint32_t heapFreeBytes = ESP.getFreeHeap();
  pageTurnPending = false;
  logSerial.printf(
      "PERF {\"v\":2,\"scenario\":\"page_turn_in_section\",\"iteration\":%lu,\"duration_us\":%lu,"
      "\"direction\":\"%s\",\"spine_index\":%lu,\"from_page\":%lu,\"to_page\":%lu,"
      "\"font_size\":%u,\"text_antialiasing\":%s,\"refresh_mode\":\"%s\","
      "\"heap_free_bytes\":%lu}\n",
      static_cast<unsigned long>(pageTurnIteration), static_cast<unsigned long>(durationUs),
      pageTurnForward ? "forward" : "backward", static_cast<unsigned long>(pageTurnSpineIndex),
      static_cast<unsigned long>(pageTurnFromPage), static_cast<unsigned long>(pageTurnToPage),
      static_cast<unsigned>(pageTurnFontSize), pageTurnTextAntialiasing ? "true" : "false",
      refreshModeName(pageTurnRefreshMode), static_cast<unsigned long>(heapFreeBytes));
}

}  // namespace PerformanceBenchmark

#endif  // ENABLE_PERF_BENCHMARK
