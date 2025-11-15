// Simplified standalone tests for duplicate frame removal logic.
//
// - No SWELL/GUI usage
// - Uses a minimal in-memory bitmap that implements LICE_IBitmap
// - Provides a lightweight LICE_BitmapCmpEx implementation for linking
//
// Build:
//   c++ -std=c++11 -O2 -I . -I WDL \
//       licecap/test_duplicate_simple.cpp licecap/duplicate_frame_removal.cpp \
//       -o test_simple

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <cmath>
#include <algorithm>

#include "licecap/duplicate_frame_removal.h"

// ---------------------------------------------------------------------
// Minimal in-memory bitmap implementing LICE_IBitmap
// ---------------------------------------------------------------------

class SimpleBitmap : public LICE_IBitmap
{
public:
  SimpleBitmap(int w=0, int h=0) { resize(w,h); }
  virtual ~SimpleBitmap() {}

  virtual LICE_pixel* getBits() { return data_.empty() ? nullptr : &data_[0]; }
  virtual int getWidth() { return w_; }
  virtual int getHeight() { return h_; }
  virtual int getRowSpan() { return row_span_; }
  virtual bool resize(int w, int h)
  {
    if (w < 0) w = 0; if (h < 0) h = 0;
    w_ = w; h_ = h; row_span_ = w_ > 0 ? w_ : 0; // tightly packed rows
    data_.assign((size_t)row_span_ * (size_t)h_, LICE_RGBA(0,0,0,0));
    return true;
  }
  virtual bool isFlipped() { return false; }

  void fill(LICE_pixel px)
  {
    std::fill(data_.begin(), data_.end(), px);
  }

  void setPixel(int x, int y, LICE_pixel px)
  {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    data_[(size_t)y * (size_t)row_span_ + (size_t)x] = px;
  }

  LICE_pixel getPixel(int x, int y) const
  {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return LICE_RGBA(0,0,0,0);
    return data_[(size_t)y * (size_t)row_span_ + (size_t)x];
  }

private:
  int w_ = 0, h_ = 0, row_span_ = 0;
  std::vector<LICE_pixel> data_;
};

// ---------------------------------------------------------------------
// Lightweight implementation of LICE_BitmapCmpEx for linking & tests
// Returns 0 if identical (under mask), nonzero otherwise.
// If coordsOut != NULL, writes {x,y,w,h} bounding box of differences.
// ---------------------------------------------------------------------

int LICE_BitmapCmpEx(LICE_IBitmap* a, LICE_IBitmap* b, LICE_pixel mask, int *coordsOut)
{
  if (!a || !b) {
    if (coordsOut) { coordsOut[0]=0; coordsOut[1]=0; coordsOut[2]=0; coordsOut[3]=0; }
    return 1;
  }

  const int aw = a->getWidth();
  const int ah = a->getHeight();
  const int bw = b->getWidth();
  const int bh = b->getHeight();
  if (aw != bw || ah != bh) {
    if (coordsOut) { coordsOut[0]=0; coordsOut[1]=0; coordsOut[2]= (aw>bw?aw:bw); coordsOut[3]= (ah>bh?ah:bh); }
    return 1;
  }

  const LICE_pixel* p1 = a->getBits();
  const LICE_pixel* p2 = b->getBits();
  int rs1 = a->getRowSpan();
  int rs2 = b->getRowSpan();
  if (!p1 || !p2 || aw<=0 || ah<=0) {
    if (coordsOut) { coordsOut[0]=0; coordsOut[1]=0; coordsOut[2]=0; coordsOut[3]=0; }
    return 0;
  }

  int minx = aw, miny = ah, maxx = -1, maxy = -1;
  for (int y=0; y<ah; ++y)
  {
    const LICE_pixel* r1 = p1 + y*rs1;
    const LICE_pixel* r2 = p2 + y*rs2;
    for (int x=0; x<aw; ++x)
    {
      if ( ((r1[x] ^ r2[x]) & mask) != 0 )
      {
        if (x < minx) minx = x;
        if (y < miny) miny = y;
        if (x > maxx) maxx = x;
        if (y > maxy) maxy = y;
      }
    }
  }

  if (maxx < minx || maxy < miny)
  {
    if (coordsOut) { coordsOut[0]=0; coordsOut[1]=0; coordsOut[2]=0; coordsOut[3]=0; }
    return 0; // identical
  }

  if (coordsOut)
  {
    coordsOut[0] = minx;
    coordsOut[1] = miny;
    coordsOut[2] = (maxx - minx + 1);
    coordsOut[3] = (maxy - miny + 1);
  }
  return 1;
}

// ---------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------

static int g_failures = 0;

static void expect_true(bool cond, const char* msg)
{
  if (!cond) {
    ++g_failures;
    fprintf(stderr, "FAIL: %s\n", msg);
  }
}

static void expect_close(double a, double b, double eps, const char* msg)
{
  if (std::fabs(a-b) > eps) {
    ++g_failures;
    fprintf(stderr, "FAIL: %s (got=%g, want=%g)\n", msg, a, b);
  }
}

static SimpleBitmap make_solid(int w, int h, LICE_pixel px)
{
  SimpleBitmap bm(w,h);
  bm.fill(px);
  return bm;
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

static void test_similarity_basic()
{
  DuplicateFrameRemovalSettings cfg; // defaults: exact RGB, ignore A
  SimpleBitmap a = make_solid(16,16, LICE_RGBA(10,20,30,40));
  SimpleBitmap b = make_solid(16,16, LICE_RGBA(10,20,30,0)); // alpha ignored

  double s1 = CalculateSimilarity(&a, &b, NULL, cfg);
  expect_close(s1, 1.0, 1e-12, "identical under RGB mask should be 1.0");

  // Change one pixel's blue channel
  b.setPixel(3,4, LICE_RGBA(10,20,31,0));
  double s2 = CalculateSimilarity(&a, &b, NULL, cfg);
  const double expected = 1.0 - 1.0 / (16.0*16.0);
  expect_close(s2, expected, 1e-9, "single-pixel difference similarity");

  // ROI excluding the changed pixel should yield 1.0
  RECT roi = {0,0,3,4};
  double s3 = CalculateSimilarity(&a, &b, &roi, cfg);
  expect_close(s3, 1.0, 1e-12, "ROI excluding diff should be 1.0");
}

static void test_similarity_tolerance_and_mask()
{
  SimpleBitmap a = make_solid(8,8, LICE_RGBA(100,100,100,255));
  SimpleBitmap b = make_solid(8,8, LICE_RGBA(101,100,100,10)); // R+1, alpha ignored

  DuplicateFrameRemovalSettings cfg;
  cfg.per_channel_tolerance = 1;
  cfg.channel_mask = LICE_RGBA(255,255,255,0); // RGB
  double s = CalculateSimilarity(&a, &b, NULL, cfg);
  expect_close(s, 1.0, 1e-12, "tolerance=1 allows R+1 change");

  // Ignore blue channel entirely with strict compare
  SimpleBitmap c = make_solid(8,8, LICE_RGBA(10,20,30,0));
  SimpleBitmap d = make_solid(8,8, LICE_RGBA(10,20,35,200)); // blue differs
  DuplicateFrameRemovalSettings cfg2;
  cfg2.per_channel_tolerance = 0; // strict
  cfg2.channel_mask = LICE_RGBA(255,255,0,0); // ignore blue
  int diffs[4] = {0,0,0,0};
  int rc = LICE_BitmapCmpEx(&c, &d, cfg2.channel_mask, diffs);
  expect_true(rc == 0, "LICE_BitmapCmpEx ignores blue difference with mask");
  double s2 = CalculateSimilarity(&c, &d, NULL, cfg2);
  expect_close(s2, 1.0, 1e-12, "channel_mask ignores blue in strict compare");
}

static void test_similarity_sampling()
{
  SimpleBitmap a = make_solid(10,10, LICE_RGBA(0,0,0,0));
  SimpleBitmap b = make_solid(10,10, LICE_RGBA(0,0,0,0));
  // Change pixel at (1,1) which will be skipped by 2x2 sampling from origin
  b.setPixel(1,1, LICE_RGBA(255,255,255,255));

  DuplicateFrameRemovalSettings cfg;
  cfg.sample_step_x = 2;
  cfg.sample_step_y = 2;
  double s = CalculateSimilarity(&a, &b, NULL, cfg);
  expect_close(s, 1.0, 1e-12, "sampling skips unsampled differences");
}

static void test_is_duplicate_logic()
{
  SimpleBitmap a = make_solid(20,20, LICE_RGBA(5,6,7,0));
  SimpleBitmap b = make_solid(20,20, LICE_RGBA(5,6,7,0));
  SimpleBitmap c = make_solid(20,20, LICE_RGBA(5,6,8,0)); // small diff

  FrameInfo f0(0, &a, 50);
  FrameInfo f1(1, &b, 60);
  FrameInfo f2(2, &c, 70);

  DuplicateFrameRemovalSettings cfg;
  cfg.similarity_threshold = 0.9999; // very strict
  double sim01 = 0.0, sim12 = 0.0;
  bool d01 = IsDuplicateFrame(f0, f1, cfg, &sim01);
  bool d12 = IsDuplicateFrame(f1, f2, cfg, &sim12);

  expect_true(d01 && sim01 == 1.0, "identical frames are duplicates");
  expect_true(!d12 && sim12 < 1.0, "different frames are not duplicates at strict threshold");
}

static void test_remove_duplicates_keep_first_sum()
{
  SimpleBitmap a = make_solid(12,12, LICE_RGBA(1,2,3,0));
  SimpleBitmap b = make_solid(12,12, LICE_RGBA(1,2,3,0)); // dup of a
  SimpleBitmap c = make_solid(12,12, LICE_RGBA(9,9,9,0));

  std::vector<FrameInfo> in = {
    FrameInfo(0,&a,50), FrameInfo(1,&b,50), FrameInfo(2,&c,100)
  };

  DuplicateFrameRemovalSettings cfg;
  cfg.keep_mode = kDuplicateKeepFirst;
  cfg.delay_adjust_mode = kSum;
  cfg.similarity_threshold = 0.9999;

  std::vector<FrameInfo> out;
  std::vector<size_t> removed_idx;
  size_t removed = RemoveDuplicateFrames(in, out, cfg, &removed_idx);

  expect_true(removed == 1, "one duplicate removed");
  expect_true(out.size() == 2, "two frames remain");
  expect_true(out[0].delay_ms == 100, "delay sum across duplicate run");
}

static void test_remove_duplicates_keep_last_average()
{
  SimpleBitmap a = make_solid(8,8, LICE_RGBA(4,5,6,0));
  // Three identical frames with varying delays
  std::vector<FrameInfo> in = {
    FrameInfo(0,&a,30), FrameInfo(1,&a,60), FrameInfo(2,&a,90)
  };

  DuplicateFrameRemovalSettings cfg;
  cfg.keep_mode = kDuplicateKeepLast;
  cfg.delay_adjust_mode = kAverage;
  cfg.similarity_threshold = 0.9999;

  std::vector<FrameInfo> out;
  size_t removed = RemoveDuplicateFrames(in, out, cfg, nullptr);

  expect_true(removed == 2, "two duplicates removed in run of three");
  expect_true(out.size() == 1, "one frame remains");
  expect_true(out[0].delay_ms == (30+60+90)/3, "delay averaged across run");
}

static void test_boundary_conditions()
{
  // Null and size mismatch
  DuplicateFrameRemovalSettings cfg;

  double s_null = CalculateSimilarity(nullptr, nullptr, NULL, cfg);
  expect_close(s_null, 0.0, 1e-12, "null bitmaps similarity is 0.0");

  SimpleBitmap a = make_solid(4,4, LICE_RGBA(0,0,0,0));
  SimpleBitmap b = make_solid(5,4, LICE_RGBA(0,0,0,0));
  double s_sz = CalculateSimilarity(&a, &b, NULL, cfg);
  expect_close(s_sz, 0.0, 1e-12, "different sizes similarity is 0.0");

  // Empty ROI yields 1.0
  RECT roi = {2,2,2,5};
  double s_empty = CalculateSimilarity(&a, &a, &roi, cfg);
  expect_close(s_empty, 1.0, 1e-12, "empty ROI treated as identical");

  // IsDuplicateFrame with null
  FrameInfo fnull_prev; fnull_prev.bmp = nullptr; fnull_prev.delay_ms = 10;
  FrameInfo fnull_cur(1,&a,10);
  double sim = -1.0;
  bool isdup = IsDuplicateFrame(fnull_prev, fnull_cur, cfg, &sim);
  expect_true(!isdup && sim == 0.0, "null prev is not duplicate, sim=0.0");
}

int main()
{
  test_similarity_basic();
  test_similarity_tolerance_and_mask();
  test_similarity_sampling();
  test_is_duplicate_logic();
  test_remove_duplicates_keep_first_sum();
  test_remove_duplicates_keep_last_average();
  test_boundary_conditions();

  if (g_failures == 0)
  {
    printf("All simplified tests passed.\n");
    return 0;
  }
  else
  {
    printf("%d test(s) failed.\n", g_failures);
    return 1;
  }
}
