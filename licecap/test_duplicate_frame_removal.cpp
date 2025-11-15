// test_duplicate_frame_removal.cpp
//
// Test suite for licecap duplicate frame removal and similarity functions.
//
// Build tips:
//   macOS/Linux (clang++/g++):
//     c++ -std=c++11 -O2 -I. -I WDL \
//       licecap/test_duplicate_frame_removal.cpp \
//       licecap/duplicate_frame_removal.cpp \
//       WDL/lice/lice.cpp \
//       -o test_dup
//
//   Windows (MSVC Developer Command Prompt):
//     cl /EHsc /O2 /I . /I WDL \
//       licecap\test_duplicate_frame_removal.cpp \
//       licecap\duplicate_frame_removal.cpp \
//       WDL\lice\lice.cpp \
//       /Fe:test_dup.exe
//
// Running:
//   ./test_dup
//   Outputs assertion failures (if any) and benchmarks.

#include <stdio.h>
#include <math.h>
#include "test_duplicate_frame_removal.h"

// Utilities ----------------------------------------------------------------

static void PrintCfg(const DuplicateFrameRemovalSettings& c)
{
  printf("cfg{thr=%.5f, step=%dx%d, tol=%d, keep=%d, delay=%d, early=%d}\n",
    c.similarity_threshold, c.sample_step_x, c.sample_step_y,
    c.per_channel_tolerance, (int)c.keep_mode, (int)c.delay_adjust_mode,
    c.enable_early_out?1:0);
}

// 1) Similarity accuracy tests ---------------------------------------------

TEST_CASE(Test_Similarity_IdenticalVsDifferent)
{
  GetTestStats().tests_total++;
  DuplicateFrameRemovalSettings cfg;

  LICE_MemBitmap* a = CreateBitmap(64,64, LICE_RGBA(10,20,30,0));
  LICE_MemBitmap* b = CreateBitmap(64,64, LICE_RGBA(10,20,30,0));
  LICE_MemBitmap* c = CreateBitmap(64,64, LICE_RGBA(200,100,50,0));
  TEST_ASSERT(a && b && c);

  double s_ab = CalculateSimilarity(a,b,NULL,cfg);
  double s_ac = CalculateSimilarity(a,c,NULL,cfg);

  TEST_ASSERT_NEAR(s_ab, 1.0, 1e-12);
  TEST_ASSERT_NEAR(s_ac, 0.0, 1e-9);

  delete a; delete b; delete c;
}

TEST_CASE(Test_Similarity_PartialRegion)
{
  GetTestStats().tests_total++;
  DuplicateFrameRemovalSettings cfg; // defaults: exact compare RGB, ignore A
  cfg.sample_step_x = 1; cfg.sample_step_y = 1;

  const int W = 80, H = 60;
  LICE_MemBitmap* a = CreateBitmap(W,H, LICE_RGBA(0,0,0,0));
  LICE_MemBitmap* b = CreateBitmap(W,H, LICE_RGBA(0,0,0,0));
  TEST_ASSERT(a && b);

  // Modify a vertical stripe of width 16 in b
  const int stripe_w = 16;
  FillRect(b, 0, 0, stripe_w, H, LICE_RGBA(255,255,255,0));

  const double expected = 1.0 - (double)(stripe_w*H)/(double)(W*H);
  const double s = CalculateSimilarity(a,b,NULL,cfg);

  TEST_ASSERT_NEAR(s, expected, 1e-9);

  // ROI test: compare only the stripe region (should be 0 similarity)
  RECT roi; roi.left=0; roi.top=0; roi.right=stripe_w; roi.bottom=H;
  double s_roi = CalculateSimilarity(a,b,&roi,cfg);
  TEST_ASSERT_NEAR(s_roi, 0.0, 1e-12);

  delete a; delete b;
}

TEST_CASE(Test_Similarity_ChannelMask_And_Tolerance)
{
  GetTestStats().tests_total++;
  LICE_MemBitmap* a = CreateBitmap(32,32, LICE_RGBA(100,100,100,0));
  LICE_MemBitmap* b = CreateBitmap(32,32, LICE_RGBA(100,100,100,200));
  TEST_ASSERT(a && b);

  // Default mask ignores alpha; should be identical
  DuplicateFrameRemovalSettings cfg;
  double s0 = CalculateSimilarity(a,b,NULL,cfg);
  TEST_ASSERT_NEAR(s0, 1.0, 1e-12);

  // Include alpha channel; now they differ everywhere
  cfg.channel_mask = LICE_RGBA(255,255,255,255);
  cfg.per_channel_tolerance = 0;
  double s1 = CalculateSimilarity(a,b,NULL,cfg);
  TEST_ASSERT_NEAR(s1, 0.0, 1e-9);

  // Small RGB difference with tolerance
  FillRect(b, 0,0, b->getWidth(), b->getHeight(), LICE_RGBA(102,100,100,200));
  cfg.channel_mask = LICE_RGBA(255,255,255,0); // RGB only
  cfg.per_channel_tolerance = 2;
  double s2 = CalculateSimilarity(a,b,NULL,cfg);
  TEST_ASSERT_NEAR(s2, 1.0, 1e-12);

  delete a; delete b;
}

TEST_CASE(Test_Similarity_Sampling_And_EmptyROI)
{
  GetTestStats().tests_total++;
  LICE_MemBitmap* a = CreateBitmap(50,50, LICE_RGBA(0,0,0,0));
  LICE_MemBitmap* b = CreateBitmap(50,50, LICE_RGBA(0,0,0,0));
  TEST_ASSERT(a && b);

  // Paint a small 5x5 block different
  FillRect(b, 10,10, 5,5, LICE_RGBA(255,0,0,0));

  DuplicateFrameRemovalSettings cfg;
  cfg.sample_step_x = 3; // sampling may not hit all pixels
  cfg.sample_step_y = 3;
  double s = CalculateSimilarity(a,b,NULL,cfg);
  TEST_ASSERT(s >= 0.0 && s <= 1.0);

  // Empty ROI => identical
  RECT roi; roi.left = roi.right = 10; roi.top = roi.bottom = 10;
  double se = CalculateSimilarity(a,b,&roi,cfg);
  TEST_ASSERT_NEAR(se, 1.0, 1e-12);

  delete a; delete b;
}

// 2) Duplicate detection boundary tests -----------------------------------

TEST_CASE(Test_IsDuplicate_Nulls_And_SizeMismatch)
{
  GetTestStats().tests_total++;
  DuplicateFrameRemovalSettings cfg;

  FrameInfo f1(0, NULL, 100);
  FrameInfo f2(1, NULL, 100);
  double sim = 123.0;
  TEST_ASSERT(!IsDuplicateFrame(f1,f2,cfg,&sim));
  TEST_ASSERT_NEAR(sim, 0.0, 1e-12);

  LICE_MemBitmap* a = CreateBitmap(40,40, LICE_RGBA(10,0,0,0));
  LICE_MemBitmap* b = CreateBitmap(41,40, LICE_RGBA(10,0,0,0));
  FrameInfo fa(0,a,100), fb(1,b,100);
  TEST_ASSERT(!IsDuplicateFrame(fa,fb,cfg,&sim));

  delete a; delete b;
}

TEST_CASE(Test_IsDuplicate_ROI_And_Thresholds)
{
  GetTestStats().tests_total++;
  LICE_MemBitmap* a = CreateBitmap(60,60, LICE_RGBA(0,0,0,0));
  LICE_MemBitmap* b = CreateBitmap(60,60, LICE_RGBA(0,0,0,0));
  TEST_ASSERT(a && b);

  // Change a small 10x10 block in b
  FillRect(b, 20,20, 10,10, LICE_RGBA(255,255,255,0));

  FrameInfo fa(0,a, 100), fb(1,b, 120);
  fb.x=20; fb.y=20; fb.w=10; fb.h=10; // limit comparison to changed area

  DuplicateFrameRemovalSettings cfg;
  cfg.similarity_threshold = 1.0; // exact
  double sim = 0.0;
  // In ROI they are completely different => not duplicate
  TEST_ASSERT(!IsDuplicateFrame(fa,fb,cfg,&sim));
  TEST_ASSERT_NEAR(sim, 0.0, 1e-12);

  // If threshold is 0, always duplicate
  cfg.similarity_threshold = 0.0;
  TEST_ASSERT(IsDuplicateFrame(fa,fb,cfg,&sim));

  // If we restrict ROI to empty area, should be duplicate for any threshold
  fb.x=0; fb.y=0; fb.w=0; fb.h=0; // empty => identity
  cfg.similarity_threshold = 1.0;
  TEST_ASSERT(IsDuplicateFrame(fa,fb,cfg,&sim));

  delete a; delete b;
}

// 3) Removal configuration tests ------------------------------------------

static FrameInfo FI(int idx, LICE_IBitmap* bmp, int delay) { return FrameInfo(idx,bmp,delay); }

TEST_CASE(Test_RemoveDuplicates_KeepFirst_SumDelay)
{
  GetTestStats().tests_total++;
  // Build A, A, B, B, B, C sequence
  LICE_MemBitmap* A = CreateBitmap(32,32, LICE_RGBA(10,10,10,0));
  LICE_MemBitmap* B = CreateBitmap(32,32, LICE_RGBA(20,20,20,0));
  LICE_MemBitmap* C = CreateBitmap(32,32, LICE_RGBA(30,30,30,0));
  TEST_ASSERT(A && B && C);

  std::vector<FrameInfo> in;
  in.push_back(FI(0,A,100)); // A
  in.push_back(FI(1,A,110)); // A dup
  in.push_back(FI(2,B,120)); // B
  in.push_back(FI(3,B,130)); // B dup
  in.push_back(FI(4,B,140)); // B dup
  in.push_back(FI(5,C,150)); // C

  DuplicateFrameRemovalSettings cfg; // default threshold
  cfg.keep_mode = kDuplicateKeepFirst;
  cfg.delay_adjust_mode = kSum;

  std::vector<FrameInfo> out;
  std::vector<size_t> removed;
  size_t nrem = RemoveDuplicateFrames(in,out,cfg,&removed);

  TEST_ASSERT(nrem == 3);
  TEST_ASSERT(out.size() == 3);
  TEST_ASSERT(out[0].bmp == A && out[0].delay_ms == 210); // 100+110
  TEST_ASSERT(out[1].bmp == B && out[1].delay_ms == 390); // 120+130+140
  TEST_ASSERT(out[2].bmp == C && out[2].delay_ms == 150);
  TEST_ASSERT(removed.size() == 3);
  TEST_ASSERT(removed[0] == 1 && removed[1] == 3 && removed[2] == 4);

  delete A; delete B; delete C;
}

TEST_CASE(Test_RemoveDuplicates_KeepLast_AverageDelay)
{
  GetTestStats().tests_total++;
  LICE_MemBitmap* A = CreateBitmap(16,16, LICE_RGBA(1,1,1,0));
  LICE_MemBitmap* B = CreateBitmap(16,16, LICE_RGBA(2,2,2,0));
  TEST_ASSERT(A && B);

  std::vector<FrameInfo> in;
  in.push_back(FI(0,A,10));
  in.push_back(FI(1,A,20)); // dup
  in.push_back(FI(2,B,30));
  in.push_back(FI(3,B,40)); // dup

  DuplicateFrameRemovalSettings cfg;
  cfg.keep_mode = kDuplicateKeepLast;
  cfg.delay_adjust_mode = kAverage;

  std::vector<FrameInfo> out;
  std::vector<size_t> rem;
  size_t nrem = RemoveDuplicateFrames(in,out,cfg,&rem);

  TEST_ASSERT(nrem == 2);
  TEST_ASSERT(out.size() == 2);
  // keep last of each group; average of (10,20) => 15, (30,40)=>35
  TEST_ASSERT(out[0].bmp == A && out[0].index == 1 && out[0].delay_ms == 15);
  TEST_ASSERT(out[1].bmp == B && out[1].index == 3 && out[1].delay_ms == 35);
  // Removed index list: should include the first of each pair (0 and 2)
  TEST_ASSERT(rem.size() == 2 && rem[0] == 0 && rem[1] == 2);

  delete A; delete B;
}

TEST_CASE(Test_RemoveDuplicates_NoAdjust_And_Threshold)
{
  GetTestStats().tests_total++;
  LICE_MemBitmap* A = CreateBitmap(20,20, LICE_RGBA(9,9,9,0));
  LICE_MemBitmap* B = CreateBitmap(20,20, LICE_RGBA(9,9,9,0)); // identical to A
  LICE_MemBitmap* C = CreateBitmap(20,20, LICE_RGBA(9,10,9,0)); // slightly different
  TEST_ASSERT(A && B && C);

  std::vector<FrameInfo> in;
  in.push_back(FI(0,A,10));
  in.push_back(FI(1,B,20)); // dup to A
  in.push_back(FI(2,C,30)); // small diff to A

  DuplicateFrameRemovalSettings cfg;
  cfg.keep_mode = kDuplicateKeepFirst;
  cfg.delay_adjust_mode = kDontAdjust;
  cfg.similarity_threshold = 0.99999; // near exact, A~B dup, C not dup

  std::vector<FrameInfo> out;
  size_t nrem = RemoveDuplicateFrames(in,out,cfg,NULL);
  TEST_ASSERT(nrem == 1);
  TEST_ASSERT(out.size() == 2);
  TEST_ASSERT(out[0].bmp == A && out[0].delay_ms == 10);
  TEST_ASSERT(out[1].bmp == C && out[1].delay_ms == 30);

  delete A; delete B; delete C;
}

// 4) Performance tests -----------------------------------------------------

TEST_CASE(Test_Perf_Similarity_Basics)
{
  GetTestStats().tests_total++;
  const int W = 800, H = 600;
  LICE_MemBitmap* a = CreateBitmap(W,H, LICE_RGBA(0,0,0,0));
  LICE_MemBitmap* b = CreateBitmap(W,H, LICE_RGBA(0,0,0,0));
  TEST_ASSERT(a && b);

  // Introduce a big changed block so early-out can trigger
  FillRect(b, 0, 0, W/2, H, LICE_RGBA(255,255,255,0));

  DuplicateFrameRemovalSettings cfg_fast;
  cfg_fast.sample_step_x = 2;
  cfg_fast.sample_step_y = 2;
  cfg_fast.similarity_threshold = 0.99;
  cfg_fast.enable_early_out = true;

  {
    ScopedTimer t("similarity 800x600 step2 early-out");
    volatile double s = 0.0;
    for (int i = 0; i < 10; ++i) s += CalculateSimilarity(a,b,NULL,cfg_fast);
    (void)s;
  }

  cfg_fast.enable_early_out = false;
  {
    ScopedTimer t("similarity 800x600 step2 no-early");
    volatile double s = 0.0;
    for (int i = 0; i < 10; ++i) s += CalculateSimilarity(a,b,NULL,cfg_fast);
    (void)s;
  }

  delete a; delete b;
}

// 5) Memory safety tests ---------------------------------------------------

TEST_CASE(Test_MemorySafety_NoExternalFrees_And_Immutability)
{
  GetTestStats().tests_total++;
  LICE_MemBitmap* A = CreateBitmap(64,64, LICE_RGBA(1,2,3,4));
  LICE_MemBitmap* B = CreateBitmap(64,64, LICE_RGBA(1,2,3,4));
  LICE_MemBitmap* C = CreateBitmap(64,64, LICE_RGBA(9,9,9,9));
  TEST_ASSERT(A && B && C);

  uint64_t cA = PixelChecksum(A);
  uint64_t cB = PixelChecksum(B);
  uint64_t cC = PixelChecksum(C);

  // Ensure similarity/remove calls do not modify inputs
  DuplicateFrameRemovalSettings cfg;
  (void)CalculateSimilarity(A,B,NULL,cfg);
  std::vector<FrameInfo> in, out; std::vector<size_t> rem;
  in.push_back(FI(0,A,10)); in.push_back(FI(1,B,20)); in.push_back(FI(2,C,30));
  (void)RemoveDuplicateFrames(in,out,cfg,&rem);

  TEST_ASSERT(PixelChecksum(A) == cA);
  TEST_ASSERT(PixelChecksum(B) == cB);
  TEST_ASSERT(PixelChecksum(C) == cC);

  // Ensure pointers in output are original pointers (no ownership change)
  for (size_t i = 0; i < out.size(); ++i) {
    TEST_ASSERT(out[i].bmp == A || out[i].bmp == B || out[i].bmp == C);
  }

  delete A; delete B; delete C;
}

// 6) Edge conditions -------------------------------------------------------

TEST_CASE(Test_Edges_EmptyInput_And_Steps)
{
  GetTestStats().tests_total++;
  std::vector<FrameInfo> in, out; std::vector<size_t> rem;
  DuplicateFrameRemovalSettings cfg;
  size_t nrem = RemoveDuplicateFrames(in,out,cfg,&rem);
  TEST_ASSERT(nrem == 0);
  TEST_ASSERT(out.empty());
  TEST_ASSERT(rem.empty());

  // Sample steps <= 0 should be clamped to 1 and not crash
  LICE_MemBitmap* a = CreateBitmap(10,10, LICE_RGBA(0,0,0,0));
  LICE_MemBitmap* b = CreateBitmap(10,10, LICE_RGBA(1,1,1,0));
  TEST_ASSERT(a && b);
  cfg.sample_step_x = 0; cfg.sample_step_y = -5;
  double s = CalculateSimilarity(a,b,NULL,cfg);
  TEST_ASSERT(s >= 0.0 && s <= 1.0);
  delete a; delete b;
}

// Test runner --------------------------------------------------------------

int main()
{
  printf("Running duplicate-frame removal tests...\n");
  int failed_tests = 0;
  auto& reg = GetRegistry();
  for (size_t i = 0; i < reg.size(); ++i) {
    const char* name = reg[i].name;
    printf("[ RUN      ] %s\n", name);
    int before_fail = GetTestStats().asserts_failed;
    reg[i].fn();
    int after_fail = GetTestStats().asserts_failed;
    if (after_fail == before_fail) {
      printf("[       OK ] %s\n", name);
    } else {
      printf("[  FAILED  ] %s (fail asserts: %d)\n", name, after_fail - before_fail);
      failed_tests++;
      GetTestStats().tests_failed++;
    }
  }

  printf("\nSummary:\n");
  printf("  Tests   : %d total, %d failed\n", GetTestStats().tests_total, GetTestStats().tests_failed);
  printf("  Asserts : %d total, %d failed\n", GetTestStats().asserts_total, GetTestStats().asserts_failed);

  // Nonzero exit if any assert failed
  return GetTestStats().asserts_failed ? 1 : 0;
}

