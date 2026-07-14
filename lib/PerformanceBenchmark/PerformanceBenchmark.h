#pragma once

#include <cstdint>

namespace PerformanceBenchmark {

enum class PageRefreshMode : uint8_t { UNKNOWN, FAST, HALF, IMAGE };

#ifdef ENABLE_PERF_BENCHMARK

uint32_t nowUs();
void expectBootToHome();
void recordHomePaint();
void beginBookOpen();
void setBookCacheHit(bool cacheHit);
void recordEpubLoad(uint32_t startedAtUs, bool cacheHit);
void recordReadestProbe(uint32_t startedAtUs, bool managed);
void finishBookOpen(bool managed);
void beginPageTurn(bool forward, uint32_t spineIndex, uint32_t fromPage, uint32_t toPage,
                   uint8_t fontSize, bool textAntialiasing);
void setPageTurnRefreshMode(PageRefreshMode refreshMode);
void finishPageTurn();

#else

inline uint32_t nowUs() { return 0; }
inline void expectBootToHome() {}
inline void recordHomePaint() {}
inline void beginBookOpen() {}
inline void setBookCacheHit(bool) {}
inline void recordEpubLoad(uint32_t, bool) {}
inline void recordReadestProbe(uint32_t, bool) {}
inline void finishBookOpen(bool) {}
inline void beginPageTurn(bool, uint32_t, uint32_t, uint32_t, uint8_t, bool) {}
inline void setPageTurnRefreshMode(PageRefreshMode) {}
inline void finishPageTurn() {}

#endif

}  // namespace PerformanceBenchmark
