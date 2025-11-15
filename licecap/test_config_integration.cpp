// test_config_integration.cpp
//
// Integration-style tests for LICEcap configuration persistence
// (INI save/load) and duplicate-frame removal integration in the
// capture/encoder flow — without requiring the full GUI.
//
// This test focuses on:
// - Saving duplicate-removal settings to INI (SaveConfig-equivalent)
// - Loading settings from INI with clamping/defaults
// - Interaction via the globals used by licecap_ui.cpp
// - Simulating a minimal recording flow with a gif-encoder-like class
//   that integrates duplicate detection using CalculateSimilarity()
//
// Build (examples):
//   c++ -std=c++11 -O2 -I . -I WDL \
//       licecap/test_config_integration.cpp \
//       licecap/duplicate_frame_removal.cpp \
//       WDL/swell/swell-ini.cpp \
//       -o test_config_integration
//
// Run:
//   ./test_config_integration

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <string>
#include <cmath>
#include <time.h>
#if defined(_WIN32)
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

#include "duplicate_frame_removal.h"

// ---------------------------------------------------------------------
// Globals mirroring licecap_ui.cpp for duplicate removal
// ---------------------------------------------------------------------

bool g_dupremoval_enable = false;                  // default off
DuplicateFrameRemovalSettings g_dupremoval_cfg;    // default values via ctor

// INI keys (must match licecap_ui.cpp)
static const char* kIniDupEnable = "dup_remove_enable";
static const char* kIniDupThresh = "dup_similarity";
static const char* kIniDupKeep   = "dup_keep_mode";   // 0=keep first, 1=keep last
static const char* kIniDupSx     = "dup_sample_x";
static const char* kIniDupSy     = "dup_sample_y";
static const char* kIniDupTol    = "dup_tolerance";   // per-channel tolerance
static const char* kIniDupChan   = "dup_channel_mask"; // integer mask (LICE_RGBA)
static const char* kIniDupEarly  = "dup_early_out";    // 0/1

// ---------------------------------------------------------------------
// Minimal bitmap implementation and required LICE helpers
// ---------------------------------------------------------------------

class SimpleBitmap : public LICE_IBitmap
{
public:
  explicit SimpleBitmap(int w=0, int h=0) { resize(w,h); }
  virtual ~SimpleBitmap() {}

  virtual LICE_pixel* getBits() { return data_.empty() ? nullptr : &data_[0]; }
  virtual int getWidth() { return w_; }
  virtual int getHeight() { return h_; }
  virtual int getRowSpan() { return row_span_; }
  virtual bool resize(int w, int h)
  {
    if (w < 0) w = 0; if (h < 0) h = 0;
    w_ = w; h_ = h; row_span_ = w_ > 0 ? w_ : 0;
    data_.assign((size_t)row_span_ * (size_t)h_, LICE_RGBA(0,0,0,0));
    return true;
  }
  virtual bool isFlipped() { return false; }

  void fill(LICE_pixel px)
  {
    std::fill(data_.begin(), data_.end(), px);
  }

  void blitFrom(const SimpleBitmap& src, int dstx, int dsty, int srcx, int srcy, int w, int h)
  {
    if (w <= 0 || h <= 0) return;
    for (int y=0; y<h; ++y)
    {
      int sy = srcy + y, dy = dsty + y;
      if (sy < 0 || sy >= src.h_ || dy < 0 || dy >= h_) continue;
      for (int x=0; x<w; ++x)
      {
        int sx = srcx + x, dx = dstx + x;
        if (sx < 0 || sx >= src.w_ || dx < 0 || dx >= w_) continue;
        data_[(size_t)dy * (size_t)row_span_ + (size_t)dx] =
          src.data_[(size_t)sy * (size_t)src.row_span_ + (size_t)sx];
      }
    }
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

// LICE helpers used by encoder/tests
int LICE_BitmapCmpEx(LICE_IBitmap* a, LICE_IBitmap* b, LICE_pixel mask, int *coordsOut)
{
  if (!a || !b)
  {
    if (coordsOut) { coordsOut[0]=0; coordsOut[1]=0; coordsOut[2]=0; coordsOut[3]=0; }
    return 1;
  }
  const int aw=a->getWidth(), ah=a->getHeight();
  const int bw=b->getWidth(), bh=b->getHeight();
  if (aw != bw || ah != bh)
  {
    if (coordsOut) { coordsOut[0]=0; coordsOut[1]=0; coordsOut[2]= (aw>bw?aw:bw); coordsOut[3]= (ah>bh?ah:bh); }
    return 1;
  }
  const LICE_pixel* p1 = a->getBits();
  const LICE_pixel* p2 = b->getBits();
  int rs1 = a->getRowSpan();
  int rs2 = b->getRowSpan();
  if (!p1 || !p2 || aw<=0 || ah<=0)
  {
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
    coordsOut[0]=minx; coordsOut[1]=miny; coordsOut[2]=maxx-minx+1; coordsOut[3]=maxy-miny+1;
  }
  return 1;
}

static inline SimpleBitmap* asSimple(LICE_IBitmap* b) { return static_cast<SimpleBitmap*>(b); }

LICE_IBitmap* LICE_CreateMemBitmap(int w, int h)
{
  return new SimpleBitmap(w,h);
}

void LICE_Copy(LICE_IBitmap* dst, LICE_IBitmap* src)
{
  if (!dst || !src) return;
  if (dst->getWidth() != src->getWidth() || dst->getHeight() != src->getHeight()) return;
  SimpleBitmap* d = asSimple(dst);
  SimpleBitmap* s = asSimple(src);
  d->blitFrom(*s, 0, 0, 0, 0, s->getWidth(), s->getHeight());
}

void LICE_Blit(LICE_IBitmap* dst, LICE_IBitmap* src, int dstx, int dsty, int srcx, int srcy, int w, int h, float /*alpha*/, int /*mode*/)
{
  if (!dst || !src) return;
  SimpleBitmap* d = asSimple(dst);
  SimpleBitmap* s = asSimple(src);
  d->blitFrom(*s, dstx, dsty, srcx, srcy, w, h);
}

// Stub GIF writing to capture encoder output
struct WrittenFrame { SimpleBitmap bmp; int x=0,y=0,w=0,h=0; int delay_ms=0; };
static std::vector<WrittenFrame> g_written_frames;

bool LICE_WriteGIFEnd(void* /*ctx*/) { return true; }
bool LICE_WriteGIFFrame(void* /*ctx*/, LICE_IBitmap* frame, int xpos, int ypos, bool /*perImageColorMap*/, int frame_delay, int /*nreps*/)
{
  WrittenFrame wf;
  wf.delay_ms = frame_delay;
  // Copy frame region into stored bmp
  wf.w = frame->getWidth();
  wf.h = frame->getHeight();
  wf.x = xpos;
  wf.y = ypos;
  wf.bmp.resize(wf.w, wf.h);
  SimpleBitmap* src = asSimple(frame);
  wf.bmp.blitFrom(*src, 0,0, 0,0, wf.w, wf.h);
  g_written_frames.push_back(wf);
  return true;
}

// ---------------------------------------------------------------------
// Minimal encoder that mirrors licecap_ui.cpp::gif_encoder logic
// ---------------------------------------------------------------------

class TestGifEncoder
{
  LICE_IBitmap* lastbm_ = nullptr;
  void* ctx_ = nullptr;
  int last_coords_[4] = {0,0,0,0};
  int accum_delay_ = 0;
  int loopcnt_ = 0;
  LICE_pixel trans_mask_ = LICE_RGBA(0xff,0xff,0xff,0);

  bool dup_remove_enable_ = false;
  DuplicateFrameRemovalSettings dup_cfg_;

public:
  explicit TestGifEncoder(void* ctx, int loopcnt)
  : ctx_(ctx), loopcnt_(loopcnt)
  {
    dup_remove_enable_ = g_dupremoval_enable;
    dup_cfg_ = g_dupremoval_cfg;
  }
  ~TestGifEncoder()
  {
    frame_finish();
    LICE_WriteGIFEnd(ctx_);
    delete lastbm_;
  }

  bool frame_compare(LICE_IBitmap* bm, int diffs[4])
  {
    diffs[0]=diffs[1]=0; diffs[2]=bm->getWidth(); diffs[3]=bm->getHeight();
    if (!lastbm_) return true;

    if (!dup_remove_enable_)
    {
      return LICE_BitmapCmpEx(lastbm_, bm, trans_mask_, diffs) ? true : false;
    }

    double sim = CalculateSimilarity(lastbm_, bm, NULL, dup_cfg_);
    if (sim >= dup_cfg_.similarity_threshold)
    {
      if (dup_cfg_.keep_mode == kDuplicateKeepLast)
      {
        LICE_Copy(lastbm_, bm);
      }
      return false; // duplicate, no new frame
    }
    return LICE_BitmapCmpEx(lastbm_, bm, trans_mask_, diffs) ? true : false;
  }

  void frame_finish()
  {
    if (ctx_ && lastbm_ && last_coords_[2] > 0 && last_coords_[3] > 0)
    {
      int del = accum_delay_ < 1 ? 1 : accum_delay_;
      // Write the pending sub-bitmap region of lastbm_
      // We create a temporary SimpleBitmap view with that region
      SimpleBitmap sub(last_coords_[2], last_coords_[3]);
      SimpleBitmap* src = asSimple(lastbm_);
      sub.blitFrom(*src, 0,0, last_coords_[0], last_coords_[1], last_coords_[2], last_coords_[3]);
      LICE_WriteGIFFrame(ctx_, &sub, last_coords_[0], last_coords_[1], true, del, loopcnt_);
    }
    accum_delay_ = 0;
    last_coords_[2]=last_coords_[3]=0;
  }

  void frame_advancetime(int amt) { accum_delay_ += amt; }

  void frame_new(LICE_IBitmap* ref, int x, int y, int w, int h)
  {
    if (w > 0 && h > 0)
    {
      frame_finish();
      last_coords_[0]=x; last_coords_[1]=y; last_coords_[2]=w; last_coords_[3]=h;
      if (!lastbm_) lastbm_ = LICE_CreateMemBitmap(ref->getWidth(), ref->getHeight());
      LICE_Blit(lastbm_, ref, x, y, x, y, w, h, 1.0f, 0);
    }
  }

  void clear_history() { frame_finish(); delete lastbm_; lastbm_ = nullptr; }
  LICE_IBitmap* prev_bitmap() { return lastbm_; }
};

// ---------------------------------------------------------------------
// INI helpers (minimal, test-local)
// File format: simple key=value lines (single implicit section)
// ---------------------------------------------------------------------
static void ini_write_kv(const char* path, const char* key, const char* val)
{
  std::vector<std::pair<std::string,std::string>> kv;
  {
    FILE* fp = fopen(path, "r");
    if (fp)
    {
      char line[512];
      while (fgets(line, sizeof(line), fp))
      {
        char* p = strchr(line, '\n'); if (p) *p = 0;
        p = strchr(line, '\r'); if (p) *p = 0;
        if (!line[0]) continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k(line), v(eq+1);
        kv.push_back(std::make_pair(k,v));
      }
      fclose(fp);
    }
  }
  bool updated=false;
  for (auto& it : kv)
  {
    if (it.first == key) { it.second = val ? val : ""; updated=true; break; }
  }
  if (!updated) kv.push_back(std::make_pair(std::string(key), std::string(val?val:"")));

  FILE* out = fopen(path, "w");
  if (!out) return;
  for (auto& it : kv)
  {
    fprintf(out, "%s=%s\n", it.first.c_str(), it.second.c_str());
  }
  fclose(out);
}

static std::string ini_get(const char* path, const char* key, const char* def)
{
  FILE* fp = fopen(path, "r");
  if (!fp) return def ? std::string(def) : std::string();
  char line[512];
  while (fgets(line, sizeof(line), fp))
  {
    char* p = strchr(line, '\n'); if (p) *p = 0;
    p = strchr(line, '\r'); if (p) *p = 0;
    if (!line[0]) continue;
    char* eq = strchr(line, '='); if (!eq) continue;
    *eq = 0;
    if (strcmp(line, key) == 0)
    {
      std::string v(eq+1);
      fclose(fp);
      return v;
    }
  }
  fclose(fp);
  return def ? std::string(def) : std::string();
}

static int ini_get_int(const char* path, const char* key, int def)
{
  std::string v = ini_get(path, key, nullptr);
  if (v.empty()) return def;
  char* endp = nullptr;
  long n = strtol(v.c_str(), &endp, 10);
  if (endp && *endp == 0) return (int)n;
  return def;
}

static void SaveDupConfigToIni(const char* ini_path)
{
  char buf[128];
  ini_write_kv(ini_path, kIniDupEnable, g_dupremoval_enable?"1":"0");
  snprintf(buf, sizeof(buf), "%.6f", g_dupremoval_cfg.similarity_threshold);
  ini_write_kv(ini_path, kIniDupThresh, buf);
  snprintf(buf, sizeof(buf), "%d", (int)g_dupremoval_cfg.keep_mode);
  ini_write_kv(ini_path, kIniDupKeep, buf);
  {
    int sx = g_dupremoval_cfg.sample_step_x; if (sx < 1) sx = 1;
    snprintf(buf, sizeof(buf), "%d", sx);
  }
  ini_write_kv(ini_path, kIniDupSx, buf);
  {
    int sy = g_dupremoval_cfg.sample_step_y; if (sy < 1) sy = 1;
    snprintf(buf, sizeof(buf), "%d", sy);
  }
  ini_write_kv(ini_path, kIniDupSy, buf);
  {
    int tol = g_dupremoval_cfg.per_channel_tolerance; if (tol < 0) tol = 0;
    snprintf(buf, sizeof(buf), "%d", tol);
  }
  ini_write_kv(ini_path, kIniDupTol, buf);
  snprintf(buf, sizeof(buf), "%u", (unsigned)g_dupremoval_cfg.channel_mask);
  ini_write_kv(ini_path, kIniDupChan, buf);
  ini_write_kv(ini_path, kIniDupEarly, g_dupremoval_cfg.enable_early_out?"1":"0");
}

static void LoadDupConfigFromIni(const char* ini_path)
{
  g_dupremoval_enable = !!ini_get_int(ini_path, kIniDupEnable, g_dupremoval_enable?1:0);
  {
    std::string vs = ini_get(ini_path, kIniDupThresh, "");
    if (!vs.empty()) {
      double th = atof(vs.c_str());
      if (th < 0.0) th = 0.0; else if (th > 1.0) th = 1.0;
      g_dupremoval_cfg.similarity_threshold = th;
    }
  }
  g_dupremoval_cfg.keep_mode = ini_get_int(ini_path, kIniDupKeep, (int)g_dupremoval_cfg.keep_mode) ? kDuplicateKeepLast : kDuplicateKeepFirst;
  {
    int sx = ini_get_int(ini_path, kIniDupSx, g_dupremoval_cfg.sample_step_x); if (sx < 1) sx = 1; g_dupremoval_cfg.sample_step_x = sx;
    int sy = ini_get_int(ini_path, kIniDupSy, g_dupremoval_cfg.sample_step_y); if (sy < 1) sy = 1; g_dupremoval_cfg.sample_step_y = sy;
    int tol = ini_get_int(ini_path, kIniDupTol, g_dupremoval_cfg.per_channel_tolerance); if (tol < 0) tol = 0; g_dupremoval_cfg.per_channel_tolerance = tol;
  }
  {
    std::string vs = ini_get(ini_path, kIniDupChan, "");
    if (!vs.empty()) { unsigned int cm=0; sscanf(vs.c_str(), "%u", &cm); g_dupremoval_cfg.channel_mask=(LICE_pixel)cm; }
  }
  g_dupremoval_cfg.enable_early_out = !!ini_get_int(ini_path, kIniDupEarly, g_dupremoval_cfg.enable_early_out?1:0);
}

// ---------------------------------------------------------------------
// Test harness
// ---------------------------------------------------------------------

static int g_failures = 0;
static void expect_true(bool cond, const char* msg)
{
  if (!cond) { ++g_failures; fprintf(stderr, "FAIL: %s\n", msg); }
}
static void expect_eq_int(int a, int b, const char* msg)
{
  if (a != b) { ++g_failures; fprintf(stderr, "FAIL: %s (got=%d want=%d)\n", msg, a, b); }
}
static void expect_eq_u(unsigned a, unsigned b, const char* msg)
{
  if (a != b) { ++g_failures; fprintf(stderr, "FAIL: %s (got=%u want=%u)\n", msg, a, b); }
}
static void expect_close(double a, double b, double eps, const char* msg)
{
  if (std::fabs(a-b) > eps) { ++g_failures; fprintf(stderr, "FAIL: %s (got=%g want=%g)\n", msg, a, b); }
}

static std::string make_temp_ini_path()
{
  char buf[256];
  snprintf(buf, sizeof(buf), "licecap_test_%ld_%d.ini", (long)time(NULL), (int)getpid());
  return std::string(buf);
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

static void test_ini_save_and_load_roundtrip()
{
  std::string ini = make_temp_ini_path();

  // Configure custom values and save
  g_dupremoval_enable = true;
  g_dupremoval_cfg.similarity_threshold = 0.90;
  g_dupremoval_cfg.keep_mode = kDuplicateKeepLast;
  g_dupremoval_cfg.sample_step_x = 3;
  g_dupremoval_cfg.sample_step_y = 4;
  g_dupremoval_cfg.per_channel_tolerance = 2;
  g_dupremoval_cfg.channel_mask = LICE_RGBA(255,255,255,0);
  g_dupremoval_cfg.enable_early_out = false;
  SaveDupConfigToIni(ini.c_str());

  // Verify file content via the same simple INI helpers
  expect_eq_int(ini_get_int(ini.c_str(), kIniDupEnable, 0), 1, "dup enable saved as 1");
  expect_true(!ini_get(ini.c_str(), kIniDupThresh, "").empty(), "dup thresh saved string non-empty");
  expect_eq_int(ini_get_int(ini.c_str(), kIniDupKeep, 0), 1, "dup keep saved as 1 (last)");
  expect_eq_int(ini_get_int(ini.c_str(), kIniDupSx, 0), 3, "sample x saved");
  expect_eq_int(ini_get_int(ini.c_str(), kIniDupSy, 0), 4, "sample y saved");
  expect_true(!ini_get(ini.c_str(), kIniDupChan, "").empty(), "channel mask saved string non-empty");
  expect_eq_int(ini_get_int(ini.c_str(), kIniDupEarly, 1), 0, "early-out saved as 0");

  // Reset globals to defaults and load from INI
  g_dupremoval_enable = false;
  g_dupremoval_cfg = DuplicateFrameRemovalSettings();
  LoadDupConfigFromIni(ini.c_str());

  expect_true(g_dupremoval_enable == true, "load dup enable");
  expect_close(g_dupremoval_cfg.similarity_threshold, 0.90, 1e-6, "load threshold");
  expect_true(g_dupremoval_cfg.keep_mode == kDuplicateKeepLast, "load keep last");
  expect_eq_int(g_dupremoval_cfg.sample_step_x, 3, "load sample x");
  expect_eq_int(g_dupremoval_cfg.sample_step_y, 4, "load sample y");
  expect_eq_int(g_dupremoval_cfg.per_channel_tolerance, 2, "load tolerance");
  expect_true(g_dupremoval_cfg.enable_early_out == false, "load early-out false");
}

static void test_ini_boundary_clamping()
{
  std::string ini = make_temp_ini_path();

  // Write out-of-range values
  ini_write_kv(ini.c_str(), kIniDupEnable, "1");
  ini_write_kv(ini.c_str(), kIniDupThresh, "1.5"); // >1 -> clamp to 1
  ini_write_kv(ini.c_str(), kIniDupKeep, "0");
  ini_write_kv(ini.c_str(), kIniDupSx, "0"); // <1 -> clamp to 1
  ini_write_kv(ini.c_str(), kIniDupSy, "-10");
  ini_write_kv(ini.c_str(), kIniDupTol, "-5"); // <0 -> clamp to 0
  ini_write_kv(ini.c_str(), kIniDupChan, "0");
  ini_write_kv(ini.c_str(), kIniDupEarly, "2"); // nonzero -> true

  g_dupremoval_enable = false;
  g_dupremoval_cfg = DuplicateFrameRemovalSettings();
  LoadDupConfigFromIni(ini.c_str());

  expect_true(g_dupremoval_enable == true, "enable loads true");
  expect_close(g_dupremoval_cfg.similarity_threshold, 1.0, 1e-12, "threshold clamped to 1.0");
  expect_true(g_dupremoval_cfg.keep_mode == kDuplicateKeepFirst, "keep 0 -> first");
  expect_eq_int(g_dupremoval_cfg.sample_step_x, 1, "sample x clamped to >=1");
  expect_eq_int(g_dupremoval_cfg.sample_step_y, 1, "sample y clamped to >=1");
  expect_eq_int(g_dupremoval_cfg.per_channel_tolerance, 0, "tolerance clamped to >=0");
  expect_true(g_dupremoval_cfg.enable_early_out == true, "nonzero early-out -> true");
}

static SimpleBitmap make_solid(int w, int h, LICE_pixel px)
{
  SimpleBitmap bm(w,h); bm.fill(px); return bm;
}

static void test_encoder_duplicate_integration_keep_first()
{
  g_written_frames.clear();

  // Enable duplicate removal: identical frames considered duplicates
  g_dupremoval_enable = true;
  g_dupremoval_cfg = DuplicateFrameRemovalSettings();
  g_dupremoval_cfg.similarity_threshold = 1.0; // require exact match
  g_dupremoval_cfg.keep_mode = kDuplicateKeepFirst;

  SimpleBitmap a = make_solid(16,16, LICE_RGBA(10,20,30,0));
  SimpleBitmap b = make_solid(16,16, LICE_RGBA(10,20,30,0)); // identical

  TestGifEncoder enc((void*)0x1, 0);
  int diffs[4];
  // First frame must be new
  bool new1 = enc.frame_compare(&a, diffs);
  expect_true(new1, "first frame is new");
  enc.frame_new(&a, 0,0, a.getWidth(), a.getHeight());
  enc.frame_advancetime(50);

  // Second identical frame should be suppressed
  bool new2 = enc.frame_compare(&b, diffs);
  expect_true(!new2, "identical frame suppressed as duplicate");
  enc.frame_advancetime(60);

  // Finish to write the pending frame once
  enc.frame_finish();

  expect_eq_int((int)g_written_frames.size(), 1, "only one frame written");
  expect_eq_int(g_written_frames[0].delay_ms, 110, "delay accumulated across suppressed duplicate");
}

static void test_encoder_duplicate_keep_last_updates_content()
{
  g_written_frames.clear();

  // Near-identical frames counted as duplicates due to tolerance & threshold
  g_dupremoval_enable = true;
  g_dupremoval_cfg = DuplicateFrameRemovalSettings();
  g_dupremoval_cfg.keep_mode = kDuplicateKeepLast;
  g_dupremoval_cfg.similarity_threshold = 0.9999; // tolerant
  g_dupremoval_cfg.per_channel_tolerance = 1;     // allow +/-1 per channel

  SimpleBitmap a = make_solid(10,10, LICE_RGBA(100,100,100,0));
  SimpleBitmap b = make_solid(10,10, LICE_RGBA(100,100,100,0));
  // Change a single pixel slightly (within tolerance)
  LICE_pixel bpx = LICE_RGBA(101,100,100,0);
  // Directly write the pixel via our API
  // (SimpleBitmap doesn't expose setPixel in header; use blit via temp)
  {
    SimpleBitmap one(1,1);
    one.fill(bpx);
    b.blitFrom(one, 5,5, 0,0, 1,1);
  }

  TestGifEncoder enc((void*)0x2, 0);
  int diffs[4];
  // Start with A
  expect_true(enc.frame_compare(&a, diffs), "first frame new");
  enc.frame_new(&a, 0,0, a.getWidth(), a.getHeight());
  enc.frame_advancetime(40);

  // B is considered duplicate; keep-last should update history to B
  bool new2 = enc.frame_compare(&b, diffs);
  expect_true(!new2, "near-identical considered duplicate");
  enc.frame_advancetime(20);
  enc.frame_finish(); // flush kept content

  expect_eq_int((int)g_written_frames.size(), 1, "one written frame");
  // Validate kept frame content reflects the last duplicate (pixel at 5,5)
  LICE_pixel written_px = g_written_frames[0].bmp.getPixel(5,5);
  expect_eq_u((unsigned)written_px, (unsigned)bpx, "kept frame reflects last content");
}

int main()
{
  test_ini_save_and_load_roundtrip();
  test_ini_boundary_clamping();
  test_encoder_duplicate_integration_keep_first();
  test_encoder_duplicate_keep_last_updates_content();

  if (g_failures == 0)
  {
    printf("All config integration tests passed.\n");
    return 0;
  }
  else
  {
    printf("%d test(s) failed.\n", g_failures);
    return 1;
  }
}
