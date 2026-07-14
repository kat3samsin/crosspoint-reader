#pragma once

#include <cstdint>

namespace PerformanceBenchmark {

#ifdef ENABLE_PERF_BENCHMARK

uint32_t nowUs();
void expectBootToHome();
void recordHomePaint();
void beginBookOpen();
void setBookCacheHit(bool cacheHit);
void recordEpubLoad(uint32_t startedAtUs, bool cacheHit);
void recordReadestProbe(uint32_t startedAtUs, bool managed);
void finishBookOpen(bool managed);
void beginPageTurn(bool forward);
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
inline void beginPageTurn(bool) {}
inline void finishPageTurn() {}

#endif

}  // namespace PerformanceBenchmark
