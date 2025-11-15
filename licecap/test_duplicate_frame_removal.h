// test_duplicate_frame_removal.h
//
// Helpers for duplicate-frame removal tests.
// Uses LICE to construct bitmaps and utilities to draw content,
// compute checksums, and a tiny test harness with assertions.

#ifndef TEST_DUPLICATE_FRAME_REMOVAL_H_
#define TEST_DUPLICATE_FRAME_REMOVAL_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <stdint.h>
#include <chrono>

#include "duplicate_frame_removal.h"

// Simple result struct to accumulate test statistics
struct TestStats {
  int tests_total;
  int tests_failed;
  int asserts_total;
  int asserts_failed;
  TestStats() : tests_total(0), tests_failed(0), asserts_total(0), asserts_failed(0) {}
};

// Global accessor for stats
inline TestStats& GetTestStats() {
  static TestStats s_stats;
  return s_stats;
}

// Tiny registry for tests
typedef void (*TestFn)();
struct TestCaseReg { const char* name; TestFn fn; };

inline std::vector<TestCaseReg>& GetRegistry() {
  static std::vector<TestCaseReg> s_reg;
  return s_reg;
}

struct TestAutoReg {
  TestAutoReg(const char* name, TestFn fn) {
    GetRegistry().push_back({name, fn});
  }
};

// Assertion helpers --------------------------------------------------------

#define TEST_ASSERT_MSG(cond, msg) do { \
  GetTestStats().asserts_total++; \
  if (!(cond)) { \
    GetTestStats().asserts_failed++; \
    fprintf(stderr, "ASSERT FAILED: %s\n  at %s:%d\n", msg, __FILE__, __LINE__); \
  } \
} while(0)

#define TEST_ASSERT(cond) TEST_ASSERT_MSG((cond), #cond)

#define TEST_ASSERT_NEAR(actual, expected, eps) do { \
  const double _a = (double)(actual); \
  const double _e = (double)(expected); \
  const double _d = _a - _e < 0 ? _e - _a : _a - _e; \
  char _buf[256]; \
  snprintf(_buf, sizeof(_buf), "expected %.6f +/- %.6f but got %.6f", _e, (double)(eps), _a); \
  TEST_ASSERT_MSG(_d <= (eps), _buf); \
} while(0)

#define TEST_CASE(name) \
  static void name(); \
  static TestAutoReg _areg_##name(#name, &name); \
  static void name()

// LICE helpers -------------------------------------------------------------

inline LICE_MemBitmap* CreateBitmap(int w, int h, LICE_pixel fill = LICE_RGBA(0,0,0,0))
{
  LICE_MemBitmap* bm = new LICE_MemBitmap(w, h);
  if (!bm || !bm->getBits()) return bm; // let test detect OOM
  const int span = bm->getRowSpan();
  LICE_pixel* bits = bm->getBits();
  for (int y = 0; y < h; ++y) {
    LICE_pixel* row = bits + y * span;
    for (int x = 0; x < w; ++x) row[x] = fill;
  }
  return bm;
}

inline void FillRect(LICE_IBitmap* bm, int x, int y, int w, int h, LICE_pixel col)
{
  if (!bm) return;
  const int bw = bm->getWidth();
  const int bh = bm->getHeight();
  if (w <= 0 || h <= 0) return;
  int x0 = x < 0 ? 0 : x;
  int y0 = y < 0 ? 0 : y;
  int x1 = x + w; if (x1 > bw) x1 = bw;
  int y1 = y + h; if (y1 > bh) y1 = bh;
  LICE_pixel* bits = bm->getBits();
  int span = bm->getRowSpan();
  for (int yy = y0; yy < y1; ++yy) {
    LICE_pixel* row = bits + yy * span;
    for (int xx = x0; xx < x1; ++xx) row[xx] = col;
  }
}

inline void DrawChecker(LICE_IBitmap* bm, int cell, LICE_pixel a, LICE_pixel b)
{
  if (!bm || cell <= 0) return;
  const int w = bm->getWidth();
  const int h = bm->getHeight();
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      bool sel = ((x/cell) + (y/cell)) & 1;
      LICE_pixel c = sel ? a : b;
      bm->getBits()[y * bm->getRowSpan() + x] = c;
    }
  }
}

inline uint64_t PixelChecksum(LICE_IBitmap* bm)
{
  if (!bm) return 0;
  const int w = bm->getWidth();
  const int h = bm->getHeight();
  const int span = bm->getRowSpan();
  const LICE_pixel* bits = bm->getBits();
  uint64_t acc = 1469598103934665603ull; // FNV offset basis
  for (int y = 0; y < h; ++y) {
    const LICE_pixel* row = bits + y * span;
    for (int x = 0; x < w; ++x) {
      acc ^= (uint64_t)row[x];
      acc *= 1099511628211ull;
    }
  }
  return acc;
}

// Timing helper
struct ScopedTimer {
  const char* label;
  std::chrono::high_resolution_clock::time_point start;
  ScopedTimer(const char* l) : label(l), start(std::chrono::high_resolution_clock::now()) {}
  ~ScopedTimer() {
    using namespace std::chrono;
    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();
    printf("[bench] %s: %lld ms\n", label, (long long)ms);
  }
};

// Convenience: build a default cfg and override fields via lambda
template <typename F>
DuplicateFrameRemovalSettings CfgPatch(F f) {
  DuplicateFrameRemovalSettings c; f(c); return c;
}

#endif // TEST_DUPLICATE_FRAME_REMOVAL_H_
