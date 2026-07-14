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
bool readestProbePending = false;
bool bookCacheKnown = false;
bool bookCacheHit = false;
bool pageTurnForward = true;
bool bootToHomePending = false;
uint32_t epubLoadDurationUs = 0;
uint32_t epubLoadHeapFreeBytes = 0;
uint32_t readestProbeDurationUs = 0;
uint32_t readestProbeHeapFreeBytes = 0;
bool readestProbeManaged = false;

uint32_t elapsedUs(const uint32_t startedAtUs, const uint32_t finishedAtUs) {
  // Unsigned subtraction remains correct when micros() wraps once.
  return finishedAtUs - startedAtUs;
}

const char* cacheName(const bool known, const bool hit) {
  if (!known) return "unknown";
  return hit ? "hit" : "miss";
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
      "PERF {\"v\":1,\"scenario\":\"boot_to_home\",\"iteration\":1,\"duration_us\":%lu,"
      "\"heap_free_bytes\":%lu}\n",
      static_cast<unsigned long>(durationUs), static_cast<unsigned long>(heapFreeBytes));
}

void beginBookOpen() {
  bookIteration++;
  bookOpenStartedAtUs = nowUs();
  bookOpenPending = true;
  bookCacheKnown = false;
  epubLoadPending = false;
  readestProbePending = false;
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

void recordReadestProbe(const uint32_t startedAtUs, const bool managed) {
  readestProbeDurationUs = elapsedUs(startedAtUs, nowUs());
  readestProbeHeapFreeBytes = ESP.getFreeHeap();
  readestProbeManaged = managed;
  readestProbePending = true;
}

void finishBookOpen(const bool managed) {
  if (!bookOpenPending) return;
  const uint32_t durationUs = elapsedUs(bookOpenStartedAtUs, nowUs());
  const uint32_t heapFreeBytes = ESP.getFreeHeap();
  bookOpenPending = false;
  logSerial.printf(
      "PERF {\"v\":1,\"scenario\":\"book_open\",\"iteration\":%lu,\"duration_us\":%lu,"
      "\"cache\":\"%s\",\"managed\":%s,\"heap_free_bytes\":%lu}\n",
      static_cast<unsigned long>(bookIteration), static_cast<unsigned long>(durationUs),
      cacheName(bookCacheKnown, bookCacheHit), managed ? "true" : "false",
      static_cast<unsigned long>(heapFreeBytes));
  // Flush nested phase records only after sampling the outer duration so the
  // serial writes cannot inflate book_open.
  if (epubLoadPending) {
    logSerial.printf(
        "PERF {\"v\":1,\"scenario\":\"epub_load\",\"iteration\":%lu,\"duration_us\":%lu,"
        "\"cache\":\"%s\",\"heap_free_bytes\":%lu}\n",
        static_cast<unsigned long>(bookIteration), static_cast<unsigned long>(epubLoadDurationUs),
        cacheName(bookCacheKnown, bookCacheHit), static_cast<unsigned long>(epubLoadHeapFreeBytes));
    epubLoadPending = false;
  }
  if (readestProbePending) {
    logSerial.printf(
        "PERF {\"v\":1,\"scenario\":\"readest_probe\",\"iteration\":%lu,\"duration_us\":%lu,"
        "\"managed\":%s,\"heap_free_bytes\":%lu}\n",
        static_cast<unsigned long>(bookIteration), static_cast<unsigned long>(readestProbeDurationUs),
        readestProbeManaged ? "true" : "false", static_cast<unsigned long>(readestProbeHeapFreeBytes));
    readestProbePending = false;
  }
}

void beginPageTurn(const bool forward) {
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
  pageTurnPending = true;
  pageTurnOverlapped = false;
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
      "PERF {\"v\":1,\"scenario\":\"page_turn\",\"iteration\":%lu,\"duration_us\":%lu,"
      "\"direction\":\"%s\",\"heap_free_bytes\":%lu}\n",
      static_cast<unsigned long>(pageTurnIteration), static_cast<unsigned long>(durationUs),
      pageTurnForward ? "forward" : "backward", static_cast<unsigned long>(heapFreeBytes));
}

}  // namespace PerformanceBenchmark

#endif  // ENABLE_PERF_BENCHMARK
