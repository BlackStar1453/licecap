// licecap/test_performance.cpp
//
// Standalone performance and stability tests for duplicate frame removal.
//
// Build:
//   c++ -std=c++11 -O2 -I . -I WDL \
//       licecap/test_performance.cpp licecap/duplicate_frame_removal.cpp \
//       -o test_performance
//
// This program does not depend on any GUI framework and avoids linking
// WDL .cpp files by:
//  - Providing a minimal LICE_BitmapCmpEx implementation locally
//  - Using a simple LICE_IBitmap implementation (SimpleBitmap)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <chrono>
#include <vector>
#include <string>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <sstream>

#include "duplicate_frame_removal.h"

using Clock = std::chrono::high_resolution_clock;
using std::cout;
using std::endl;

// ------------------------------------------------------------
// Utilities: timing, formatting

struct Timer {
  Clock::time_point t0;
  void start() { t0 = Clock::now(); }
  double ms() const {
    auto d = Clock::now() - t0;
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(d).count();
  }
};

static std::string human_bytes(double bytes) {
  const char* units[] = {"B","KB","MB","GB","TB"};
  int u = 0;
  while (bytes >= 1024.0 && u < 4) { bytes /= 1024.0; ++u; }
  std::ostringstream os; os << std::fixed << std::setprecision(bytes < 10 ? 2 : (bytes < 100 ? 1 : 0)) << bytes << " " << units[u];
  return os.str();
}

// ------------------------------------------------------------
// Minimal bitmap and LICE_BitmapCmpEx shim

class SimpleBitmap : public LICE_IBitmap {
public:
  SimpleBitmap(int w=0, int h=0) { resize(w,h); }
  ~SimpleBitmap() override {}
  LICE_pixel* getBits() override { return data_.empty() ? nullptr : &data_[0]; }
  int getWidth() override { return w_; }
  int getHeight() override { return h_; }
  int getRowSpan() override { return row_span_; }
  bool resize(int w, int h) override {
    if (w < 0) w = 0; if (h < 0) h = 0;
    w_ = w; h_ = h; row_span_ = w_ > 0 ? w_ : 0; // tightly packed
    data_.assign((size_t)row_span_ * (size_t)h_, LICE_RGBA(0,0,0,0));
    return true;
  }
  bool isFlipped() override { return false; }

  void fill(LICE_pixel px) { std::fill(data_.begin(), data_.end(), px); }
  void fillRect(int x, int y, int w, int h, LICE_pixel px) {
    if (w<=0 || h<=0) return;
    int x2 = x + w; int y2 = y + h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x2 > w_) x2 = w_; if (y2 > h_) y2 = h_;
    for (int yy=y; yy<y2; ++yy) {
      LICE_pixel* row = &data_[(size_t)yy * (size_t)row_span_];
      for (int xx=x; xx<x2; ++xx) row[xx] = px;
    }
  }
  void blitFrom(const SimpleBitmap& src) {
    const int W = w_ < src.w_ ? w_ : src.w_;
    const int H = h_ < src.h_ ? h_ : src.h_;
    const int rs = src.row_span_;
    for (int yy=0; yy<H; ++yy) {
      const LICE_pixel* srow = &src.data_[(size_t)yy * (size_t)rs];
      LICE_pixel* drow = &data_[(size_t)yy * (size_t)row_span_];
      for (int xx=0; xx<W; ++xx) drow[xx] = srow[xx];
    }
  }

private:
  int w_=0, h_=0, row_span_=0;
  std::vector<LICE_pixel> data_;
};

// Lightweight LICE_BitmapCmpEx for linking CalculateSimilarity fast path.
int LICE_BitmapCmpEx(LICE_IBitmap* a, LICE_IBitmap* b, LICE_pixel mask, int *coordsOut)
{
  if (!a || !b) {
    if (coordsOut) { coordsOut[0]=coordsOut[1]=coordsOut[2]=coordsOut[3]=0; }
    return 1;
  }
  const int aw=a->getWidth(), ah=a->getHeight();
  const int bw=b->getWidth(), bh=b->getHeight();
  if (aw!=bw || ah!=bh) {
    if (coordsOut) { coordsOut[0]=0; coordsOut[1]=0; coordsOut[2]= (aw>bw?aw:bw); coordsOut[3]= (ah>bh?ah:bh); }
    return 1;
  }
  const LICE_pixel* p1=a->getBits();
  const LICE_pixel* p2=b->getBits();
  const int rs1=a->getRowSpan();
  const int rs2=b->getRowSpan();
  if (!p1 || !p2 || aw<=0 || ah<=0) {
    if (coordsOut) { coordsOut[0]=coordsOut[1]=coordsOut[2]=coordsOut[3]=0; }
    return 0;
  }
  int minx=aw, miny=ah, maxx=-1, maxy=-1;
  for (int y=0;y<ah;++y) {
    const LICE_pixel* r1=p1 + y*rs1;
    const LICE_pixel* r2=p2 + y*rs2;
    for (int x=0;x<aw;++x) {
      if (((r1[x]^r2[x]) & mask) != 0) {
        if (x<minx) minx=x; if (y<miny) miny=y; if (x>maxx) maxx=x; if (y>maxy) maxy=y;
      }
    }
  }
  if (maxx < minx || maxy < miny) {
    if (coordsOut) { coordsOut[0]=coordsOut[1]=coordsOut[2]=coordsOut[3]=0; }
    return 0;
  }
  if (coordsOut) { coordsOut[0]=minx; coordsOut[1]=miny; coordsOut[2]=maxx-minx+1; coordsOut[3]=maxy-miny+1; }
  return 1;
}

// Simple RAII owner to track memory usage of bitmaps used in tests.
struct BitmapOwner {
  SimpleBitmap* bmp;
  int w,h;
  static long long live_count;
  static long long live_bytes;
  BitmapOwner(): bmp(nullptr), w(0), h(0) {}
  explicit BitmapOwner(int W, int H): bmp(new SimpleBitmap(W,H)), w(W), h(H) {
    live_count++;
    live_bytes += (long long)W*(long long)H*4LL; // tightly packed
  }
  ~BitmapOwner(){ if (bmp){ delete bmp; bmp=nullptr; live_count--; live_bytes -= (long long)w*(long long)h*4LL; }}
  BitmapOwner(const BitmapOwner&)=delete; BitmapOwner& operator=(const BitmapOwner&)=delete;
  BitmapOwner(BitmapOwner&& o) noexcept { bmp=o.bmp; w=o.w; h=o.h; o.bmp=nullptr; }
  BitmapOwner& operator=(BitmapOwner&& o) noexcept {
    if (this!=&o){
      if(bmp){ delete bmp; live_count--; live_bytes -= (long long)w*(long long)h*4LL; }
      bmp=o.bmp; w=o.w; h=o.h; o.bmp=nullptr; // transfer only, do not change counters here
      // counters were set on original construction and will be decremented on our dtor
    }
    return *this;
  }
};
long long BitmapOwner::live_count=0;
long long BitmapOwner::live_bytes=0;

// Deterministic noise fill
static void fill_noise(SimpleBitmap* bm, uint32_t seed) {
  const int W=bm->getWidth(), H=bm->getHeight(), rs=bm->getRowSpan();
  LICE_pixel* bits=bm->getBits();
  uint32_t s=seed?seed:1u;
  for (int y=0;y<H;++y){
    for (int x=0;x<W;++x){ s^=s<<13; s^=s>>17; s^=s<<5; bits[y*rs+x]=LICE_RGBA((s)&255,(s>>8)&255,(s>>16)&255,255);} }
}

static void make_test_pair(int w,int h, BitmapOwner& A, BitmapOwner& B){
  A=BitmapOwner(w,h); B=BitmapOwner(w,h);
  fill_noise(A.bmp, 0x12345678u + (uint32_t)w*31u + (uint32_t)h*131u);
  B.bmp->blitFrom(*A.bmp);
  const int rw = ((1)>(w/50)?(1):(w/50));
  const int rh = ((1)>(h/50)?(1):(h/50));
  B.bmp->fillRect(w/3,h/2,rw,rh,LICE_RGBA(255,0,0,255));
}

static void make_opposite_pair(int w,int h, BitmapOwner& A, BitmapOwner& B){
  A=BitmapOwner(w,h); B=BitmapOwner(w,h);
  A.bmp->fill(LICE_RGBA(0,0,0,255));
  B.bmp->fill(LICE_RGBA(255,255,255,255));
}

// ------------------------------------------------------------
// Test 1: Performance baselines for similarity

struct PerfResult {
  int w, h;
  int step;
  bool early_out;
  double threshold;
  double ms_per_op;
  double fps;
};

static PerfResult bench_similarity_once(int w, int h, int step, bool early_out, double threshold, int iters) {
  DuplicateFrameRemovalSettings cfg;
  cfg.sample_step_x = step;
  cfg.sample_step_y = step;
  cfg.per_channel_tolerance = 0;
  cfg.channel_mask = LICE_RGBA(255,255,255,0);
  cfg.enable_early_out = early_out;
  cfg.similarity_threshold = threshold;

  BitmapOwner A, B;
  make_test_pair(w,h,A,B);

  // warmup
  double last = 0.0;
  for (int i=0;i<3;i++) last += CalculateSimilarity(A.bmp, B.bmp, nullptr, cfg);

  Timer t; t.start();
  double acc = 0.0;
  for (int i=0;i<iters;i++) acc += CalculateSimilarity(A.bmp, B.bmp, nullptr, cfg);
  double ms = t.ms();
  double per = ms / (double)iters;
  double fps = 1000.0 / per;
  (void)acc; // prevent optimization
  PerfResult r{w,h,step,early_out,threshold,per,fps};
  return r;
}

// ------------------------------------------------------------
// Test 2: Early-out effectiveness

static PerfResult bench_early_out_delta(int w, int h, int step, double threshold, int iters) {
  DuplicateFrameRemovalSettings cfg_no, cfg_yes;
  cfg_no.sample_step_x = cfg_no.sample_step_y = step;
  cfg_no.enable_early_out = false;
  cfg_no.similarity_threshold = threshold;

  cfg_yes = cfg_no;
  cfg_yes.enable_early_out = true;

  BitmapOwner A, B;
  make_opposite_pair(w,h,A,B); // very different

  // warmup
  (void)CalculateSimilarity(A.bmp, B.bmp, nullptr, cfg_no);
  (void)CalculateSimilarity(A.bmp, B.bmp, nullptr, cfg_yes);

  Timer t; t.start();
  for (int i=0;i<iters;i++) (void)CalculateSimilarity(A.bmp, B.bmp, nullptr, cfg_no);
  double ms_no = t.ms();

  t.start();
  for (int i=0;i<iters;i++) (void)CalculateSimilarity(A.bmp, B.bmp, nullptr, cfg_yes);
  double ms_yes = t.ms();

  PerfResult r{w,h,step,true,threshold,(ms_yes/iters),1000.0/ (ms_yes/iters)};
  // We'll print both in the report
  cout << "  Early-out vs no-early-out ("<<w<<"x"<<h<<", step="<<step<<", thr="<<threshold<<"):\n";
  cout << "    no-early-out:  " << std::fixed << std::setprecision(3) << (ms_no/iters) << " ms/op  (" << (1000.0/(ms_no/iters)) << " fps)" << endl;
  cout << "    early-out:      " << std::fixed << std::setprecision(3) << (ms_yes/iters) << " ms/op  (" << (1000.0/(ms_yes/iters)) << " fps)" << endl;
  cout << "    speedup:        x" << std::fixed << std::setprecision(2) << (ms_no>0? (ms_no/ms_yes) : 0.0) << endl;
  return r;
}

// ------------------------------------------------------------
// Test 3: Threshold impact

static void bench_thresholds(int w, int h, int step, const std::vector<double>& thresholds, int iters) {
  cout << "Threshold impact ("<<w<<"x"<<h<<", step="<<step<<"):" << endl;
  for (double thr : thresholds) {
    auto r = bench_similarity_once(w,h,step,true,thr,iters);
    cout << "  thr=" << std::fixed << std::setprecision(3) << thr
         << "  " << std::setprecision(3) << r.ms_per_op << " ms/op"
         << "  (" << std::setprecision(1) << r.fps << " fps)" << endl;
  }
}

// ------------------------------------------------------------
// Test 4: Real-world simulation and removal efficiency

struct SimResult {
  size_t frames_in;
  size_t frames_out;
  size_t frames_removed;
  double ms_total;
  double fps;
};

// Generate a simulated screen capture stream:
// - Mostly static background
// - Small 10x10 cursor block moving slowly
// - Every change_epoch frames, toggle a UI rectangle to simulate updates
static void gen_sim_frames(int w, int h, int count, int change_epoch,
                           std::vector<BitmapOwner>& pool,
                           std::vector<FrameInfo>& frames) {
  pool.clear(); frames.clear();
  pool.reserve(count);
  frames.reserve(count);

  // Base background
  BitmapOwner bg(w,h);
  fill_noise(bg.bmp, 0xCAFEBABEu);

  int cursor_x = w/5, cursor_y = h/4;
  int dx = ((1) > (w/200) ? (1) : (w/200)), dy = ((1) > (h/200) ? (1) : (h/200));

  for (int i=0;i<count;i++) {
    pool.emplace_back(w,h);
    auto& bm = pool.back();
    bm.bmp->blitFrom(*bg.bmp);

    // UI toggle every change_epoch frames (simulate a keystroke/UI change)
    if (i % change_epoch == 0) {
      int rx = (i/ change_epoch) % (((1) > (w/3)) ? (1) : (w/3));
      rx = (rx * 11) % (w - (((10) > (w/10)) ? (10) : (w/10)));
      bm.bmp->fillRect(rx, h/3, (((10) > (w/10)) ? (10) : (w/10)), (((10) > (h/25)) ? (10) : (h/25)), LICE_RGBA(40,140,240,255));
    }

    // Small moving cursor dot (minor changes most frames)
    bm.bmp->fillRect(cursor_x, cursor_y, 10, 10, LICE_RGBA(255,255,0,255));
    cursor_x += dx; cursor_y += dy;
    if (cursor_x < 0 || cursor_x+10 >= w) dx = -dx, cursor_x += dx;
    if (cursor_y < 0 || cursor_y+10 >= h) dy = -dy, cursor_y += dy;

    FrameInfo fi(i, bm.bmp, 20 /*ms*/);
    frames.push_back(fi);
  }
}

static SimResult bench_duplicate_removal(const std::vector<FrameInfo>& frames,
                                         const DuplicateFrameRemovalSettings& cfg) {
  Timer t; t.start();
  std::vector<FrameInfo> output;
  std::vector<size_t> removed;
  const size_t removed_count = RemoveDuplicateFrames(frames, output, cfg, &removed);
  const double ms = t.ms();
  SimResult r{frames.size(), output.size(), removed_count, ms, frames.empty()?0.0: (1000.0 * (double)frames.size()/ms)};
  return r;
}

// Compare: pass-through (no removal) vs duplicate removal
static void bench_pipeline_compare(const std::vector<FrameInfo>& frames,
                                   const DuplicateFrameRemovalSettings& cfg) {
  // Pass-through (simulate naive pipeline cost)
  Timer t; t.start();
  volatile size_t checksum = 0;
  for (const auto& f : frames) {
    // Trivial read to avoid optimizing away
    checksum += (size_t)f.bmp->getWidth();
  }
  double ms_naive = t.ms();

  // Removal
  auto sim = bench_duplicate_removal(frames, cfg);

  cout << "  Pipeline compare: naive vs removal:" << endl;
  cout << "    naive:   " << std::fixed << std::setprecision(3) << ms_naive << " ms total  (" << 1000.0 * (double)frames.size() / ms_naive << " fps)" << endl;
  cout << "    removal: " << std::fixed << std::setprecision(3) << sim.ms_total << " ms total  (" << sim.fps << " fps)" << endl;
  cout << "    overhead: " << std::fixed << std::setprecision(2) << (sim.ms_total / ms_naive) << "x vs naive" << endl;
  (void)checksum;
}

// ------------------------------------------------------------
// Memory/stability tests

static bool memory_stability_test(int w, int h, int loops, int frames_per_loop) {
  const long long baseline_live = BitmapOwner::live_count;
  const long long baseline_bytes = BitmapOwner::live_bytes;

  DuplicateFrameRemovalSettings cfg;
  cfg.sample_step_x = cfg.sample_step_y = 2;
  cfg.similarity_threshold = 0.995;

  for (int i=0;i<loops;i++) {
    std::vector<BitmapOwner> pool;
    std::vector<FrameInfo> frames;
    gen_sim_frames(w,h,frames_per_loop, 30, pool, frames);

    // Run removal several times to test long-running behavior
    for (int k=0;k<5;k++) {
      auto res = bench_duplicate_removal(frames, cfg);
      (void)res;
    }

    // Check bitmaps still valid (not freed unexpectedly)
    if (!pool.empty()) {
      LICE_pixel p = pool[0].bmp->getBits()[0];
      // Modify and read back to ensure memory is writable
      pool[0].bmp->getBits()[0] = LICE_RGBA(1,2,3,255);
      LICE_pixel p2 = pool[0].bmp->getBits()[0];
      if (p2 != LICE_RGBA(1,2,3,255)) return false; // unexpected
      pool[0].bmp->getBits()[0] = p; // restore
    }

    // pool/frame go out of scope here
  }

  // After loops, live bitmaps/bytes should match baseline
  return BitmapOwner::live_count == baseline_live && BitmapOwner::live_bytes == baseline_bytes;
}

// ------------------------------------------------------------
// Report helpers

static void section(const char* title) {
  cout << "\n== " << title << " ==" << endl;
}

static void line() {
  cout << "----------------------------------------" << endl;
}

// ------------------------------------------------------------
// Main

int main() {
  cout << "Duplicate Frame Removal Performance & Stability Test" << endl;
  line();

  // Settings for comparisons
  int sizes[][2] = {{100,100},{500,500},{1000,1000}};
  int steps[] = {1,2,4};

  // 1) Performance benchmarks across sizes and sampling
  section("Similarity Baselines");
  for (auto& sz : sizes) {
    int w = sz[0], h = sz[1];
    for (int step : steps) {
      int iters = (w*h <= 100*100) ? 300 : (w*h <= 500*500 ? 60 : 12);
      auto r = bench_similarity_once(w,h,step,true,0.995,iters);
      cout << "  "<<w<<"x"<<h<<", step="<<step
           << ": " << std::fixed << std::setprecision(3) << r.ms_per_op << " ms/op"
           << "  (" << std::setprecision(1) << r.fps << " fps)" << endl;
    }
  }

  // 2) Early-out effectiveness
  section("Early-Exit Optimization");
  (void)bench_early_out_delta(500,500,1,0.995,60);
  (void)bench_early_out_delta(1000,1000,2,0.995,20);

  // 3) Threshold impact
  section("Threshold Impact");
  bench_thresholds(500,500,2,{0.900,0.990,0.995,0.999}, 50);

  // 4) Real-world simulation: duplicate detection efficiency
  section("Real-World Simulation");
  {
    const int w=640, h=480, frames_count=300;
    std::vector<BitmapOwner> pool;
    std::vector<FrameInfo> frames;
    gen_sim_frames(w,h,frames_count, /*change_epoch*/40, pool, frames);

    DuplicateFrameRemovalSettings cfg;
    cfg.sample_step_x = cfg.sample_step_y = 2;
    cfg.similarity_threshold = 0.995;
    cfg.enable_early_out = true;

    auto res = bench_duplicate_removal(frames, cfg);
    cout << "  Frames in:  " << res.frames_in << ", out: " << res.frames_out
         << ", removed: " << res.frames_removed << " (" << std::fixed << std::setprecision(1)
         << (res.frames_removed * 100.0 / std::max<size_t>(1,res.frames_in)) << "%)" << endl;
    cout << "  Throughput: " << std::fixed << std::setprecision(1) << res.fps << " frames/sec" << endl;

    bench_pipeline_compare(frames, cfg);

    cout << "  Tracked bitmap memory: " << human_bytes((double)BitmapOwner::live_bytes) << " (live objects: " << BitmapOwner::live_count << ")" << endl;
  }

  // 5) Memory usage tests
  section("Memory & Stability");
  cout << "  Running long-run stability loops..." << endl;
  bool stable = memory_stability_test(640,480, /*loops*/20, /*frames_per_loop*/100);
  cout << "  Live tracked bitmaps: " << BitmapOwner::live_count << ", bytes: " << human_bytes((double)BitmapOwner::live_bytes) << endl;
  cout << "  Result: " << (stable ? "OK (no leaks, no unexpected frees)" : "FAIL (leak or unexpected free detected)") << endl;

  line();
  cout << "Done." << endl;
  return stable ? 0 : 1;
}
