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
bool pageRenderActive = false;
uint32_t pageRenderSpineIndex = 0;
uint32_t pageRenderPage = 0;
portMUX_TYPE pageTurnMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t completedPageTurnCount = 0;
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

struct CompletedPageTurn {
  bool ready = false;
  uint32_t iteration = 0;
  uint32_t durationUs = 0;
  bool forward = true;
  uint32_t spineIndex = 0;
  uint32_t fromPage = 0;
  uint32_t toPage = 0;
  uint8_t fontSize = 0;
  bool textAntialiasing = false;
  PageRefreshMode refreshMode = PageRefreshMode::UNKNOWN;
};

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
  portENTER_CRITICAL(&pageTurnMux);
  if (pageTurnPending) {
    // Multiple inputs before the panel paint completes are coalesced into one
    // render. Discard that ambiguous sample instead of attributing it to the
    // latest input.
    pageTurnOverlapped = true;
    portEXIT_CRITICAL(&pageTurnMux);
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
  portEXIT_CRITICAL(&pageTurnMux);
}

void beginPageRender(const uint32_t spineIndex, const uint32_t page) {
  portENTER_CRITICAL(&pageTurnMux);
  pageRenderSpineIndex = spineIndex;
  pageRenderPage = page;
  pageRenderActive = true;
  portEXIT_CRITICAL(&pageTurnMux);
}

void setPageTurnRefreshMode(const PageRefreshMode refreshMode) {
  portENTER_CRITICAL(&pageTurnMux);
  if (pageTurnPending && pageRenderActive && pageRenderSpineIndex == pageTurnSpineIndex &&
      pageRenderPage == pageTurnToPage) {
    pageTurnRefreshMode = refreshMode;
  }
  portEXIT_CRITICAL(&pageTurnMux);
}

void finishPageTurn() {
  CompletedPageTurn completed;
  portENTER_CRITICAL(&pageTurnMux);
  const bool renderedTarget = pageRenderActive && pageRenderSpineIndex == pageTurnSpineIndex &&
                              pageRenderPage == pageTurnToPage;
  pageRenderActive = false;
  if (pageTurnPending && pageTurnOverlapped) {
    pageTurnPending = false;
    pageTurnOverlapped = false;
  } else if (pageTurnPending && renderedTarget) {
    completed.ready = true;
    completed.iteration = pageTurnIteration;
    completed.durationUs = elapsedUs(pageTurnStartedAtUs, nowUs());
    completed.forward = pageTurnForward;
    completed.spineIndex = pageTurnSpineIndex;
    completed.fromPage = pageTurnFromPage;
    completed.toPage = pageTurnToPage;
    completed.fontSize = pageTurnFontSize;
    completed.textAntialiasing = pageTurnTextAntialiasing;
    completed.refreshMode = pageTurnRefreshMode;
    pageTurnPending = false;
    completedPageTurnCount++;
  }
  portEXIT_CRITICAL(&pageTurnMux);

  // A button press can arrive while a previous queued render is still
  // finishing. A mismatched render leaves the sample pending for its target.
  if (!completed.ready) return;
  const uint32_t heapFreeBytes = ESP.getFreeHeap();
  logSerial.printf(
      "PERF {\"v\":2,\"scenario\":\"page_turn_in_section\",\"iteration\":%lu,\"duration_us\":%lu,"
      "\"direction\":\"%s\",\"spine_index\":%lu,\"from_page\":%lu,\"to_page\":%lu,"
      "\"font_size\":%u,\"text_antialiasing\":%s,\"refresh_mode\":\"%s\","
      "\"heap_free_bytes\":%lu}\n",
      static_cast<unsigned long>(completed.iteration), static_cast<unsigned long>(completed.durationUs),
      completed.forward ? "forward" : "backward", static_cast<unsigned long>(completed.spineIndex),
      static_cast<unsigned long>(completed.fromPage), static_cast<unsigned long>(completed.toPage),
      static_cast<unsigned>(completed.fontSize), completed.textAntialiasing ? "true" : "false",
      refreshModeName(completed.refreshMode), static_cast<unsigned long>(heapFreeBytes));
}

uint32_t completedPageTurns() {
  portENTER_CRITICAL(&pageTurnMux);
  const uint32_t count = completedPageTurnCount;
  portEXIT_CRITICAL(&pageTurnMux);
  return count;
}

bool hasPendingPageTurn() {
  portENTER_CRITICAL(&pageTurnMux);
  const bool pending = pageTurnPending;
  portEXIT_CRITICAL(&pageTurnMux);
  return pending;
}

void cancelPendingPageTurn() {
  portENTER_CRITICAL(&pageTurnMux);
  pageTurnPending = false;
  pageTurnOverlapped = false;
  pageRenderActive = false;
  portEXIT_CRITICAL(&pageTurnMux);
}

}  // namespace PerformanceBenchmark

#endif  // ENABLE_PERF_BENCHMARK
