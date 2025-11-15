// duplicate_frame_removal.cpp
//
// Implementation of similarity calculation and duplicate-frame removal
// for licecap. Designed to integrate with existing capture and GIF
// generation logic.

#include "duplicate_frame_removal.h"

// avoid std::min/max due to SWELL macros; use local helpers
#include <stdlib.h>  // for malloc, realloc, free

// Internal helpers ---------------------------------------------------------

static inline int clampi(int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline void compute_roi(LICE_IBitmap* a,
                               LICE_IBitmap* b,
                               const RECT* roi_in,
                               RECT* roi_out)
{
  const int aw = a ? a->getWidth()  : 0;
  const int ah = a ? a->getHeight() : 0;
  const int bw = b ? b->getWidth()  : 0;
  const int bh = b ? b->getHeight() : 0;

  const int w = (aw < bw) ? aw : bw;
  const int h = (ah < bh) ? ah : bh;

  RECT r = {0,0,w,h};
  if (roi_in)
  {
    // Clamp requested ROI to common bounds
    r.left   = clampi(roi_in->left,   0, w);
    r.top    = clampi(roi_in->top,    0, h);
    r.right  = clampi(roi_in->right,  0, w);
    r.bottom = clampi(roi_in->bottom, 0, h);
    if (r.right < r.left)   r.right = r.left;
    if (r.bottom < r.top)   r.bottom = r.top;
  }

  *roi_out = r;
}

// Pixel equality test with optional per-channel tolerance and channel mask.
// Returns true if p1 ~ p2 under cfg->
static inline bool pixels_equal(LICE_pixel p1,
                                LICE_pixel p2,
                                const DuplicateFrameRemovalSettings* cfg)
{
  if (cfg->per_channel_tolerance <= 0)
  {
    // Strict equality with channel mask (like LICE_BitmapCmpEx logic)
    return ((p1 ^ p2) & cfg->channel_mask) == 0;
  }

  // Tolerant per-channel check. Only channels selected by mask are tested.
  const int tol = cfg->per_channel_tolerance;

  if (cfg->channel_mask & LICE_RGBA(255,0,0,0))
  {
    int d = (int)LICE_GETR(p1) - (int)LICE_GETR(p2);
    if (d < 0) d = -d;
    if (d > tol) return false;
  }
  if (cfg->channel_mask & LICE_RGBA(0,255,0,0))
  {
    int d = (int)LICE_GETG(p1) - (int)LICE_GETG(p2);
    if (d < 0) d = -d;
    if (d > tol) return false;
  }
  if (cfg->channel_mask & LICE_RGBA(0,0,255,0))
  {
    int d = (int)LICE_GETB(p1) - (int)LICE_GETB(p2);
    if (d < 0) d = -d;
    if (d > tol) return false;
  }
  if (cfg->channel_mask & LICE_RGBA(0,0,0,255))
  {
    int d = (int)LICE_GETA(p1) - (int)LICE_GETA(p2);
    if (d < 0) d = -d;
    if (d > tol) return false;
  }
  return true;
}

// Dynamic array implementations -------------------------------------------

void FrameArray_Init(FrameArray* arr, size_t initial_capacity)
{
  if (!arr) return;
  arr->frames = NULL;
  arr->count = 0;
  arr->capacity = initial_capacity;
  if (initial_capacity > 0) {
    arr->frames = (FrameInfo*)malloc(initial_capacity * sizeof(FrameInfo));
  }
}

void IndexArray_Init(IndexArray* arr, size_t initial_capacity)
{
  if (!arr) return;
  arr->indices = NULL;
  arr->count = 0;
  arr->capacity = initial_capacity;
  if (initial_capacity > 0) {
    arr->indices = (size_t*)malloc(initial_capacity * sizeof(size_t));
  }
}

void FrameArray_Add(FrameArray* arr, const FrameInfo* frame)
{
  if (!arr || !frame) return;

  if (arr->count >= arr->capacity) {
    size_t new_capacity = arr->capacity == 0 ? 16 : arr->capacity * 2;
    FrameInfo* new_frames = (FrameInfo*)realloc(arr->frames, new_capacity * sizeof(FrameInfo));
    if (!new_frames) return; // allocation failed
    arr->frames = new_frames;
    arr->capacity = new_capacity;
  }

  arr->frames[arr->count] = *frame;
  arr->count++;
}

void IndexArray_Add(IndexArray* arr, size_t index)
{
  if (!arr) return;

  if (arr->count >= arr->capacity) {
    size_t new_capacity = arr->capacity == 0 ? 16 : arr->capacity * 2;
    size_t* new_indices = (size_t*)realloc(arr->indices, new_capacity * sizeof(size_t));
    if (!new_indices) return; // allocation failed
    arr->indices = new_indices;
    arr->capacity = new_capacity;
  }

  arr->indices[arr->count] = index;
  arr->count++;
}

void FrameArray_Free(FrameArray* arr)
{
  if (!arr) return;
  if (arr->frames) {
    free(arr->frames);
    arr->frames = NULL;
  }
  arr->count = 0;
  arr->capacity = 0;
}

void IndexArray_Free(IndexArray* arr)
{
  if (!arr) return;
  if (arr->indices) {
    free(arr->indices);
    arr->indices = NULL;
  }
  arr->count = 0;
  arr->capacity = 0;
}

// Public API ---------------------------------------------------------------

double CalculateSimilarity(LICE_IBitmap* a,
                           LICE_IBitmap* b,
                           const RECT* roi,
                           const DuplicateFrameRemovalSettings* cfg)
{
  // Trivial checks
  if (!a || !b) return 0.0;
  if (a->getWidth() != b->getWidth() || a->getHeight() != b->getHeight())
    return 0.0;

  RECT r;
  compute_roi(a,b,roi,&r);
  const int rw = r.right - r.left;
  const int rh = r.bottom - r.top;
  if (rw <= 0 || rh <= 0) return 1.0; // empty region treated as identical

  // Fast-path: if cfg requires exact match and sampling is full-frame,
  // use LICE_BitmapCmpEx to bypass per-pixel loop when possible.
  if (cfg->per_channel_tolerance <= 0 &&
      cfg->sample_step_x == 1 && cfg->sample_step_y == 1 &&
      r.left == 0 && r.top == 0 && r.right == a->getWidth() && r.bottom == a->getHeight())
  {
    int diffcoords[4] = {0,0,0,0};
    int ret = LICE_BitmapCmpEx((LICE_IBitmap*)a,(LICE_IBitmap*)b,cfg->channel_mask,diffcoords);
    if (ret == 0) return 1.0; // identical
    // If different, provide a rough similarity estimate by area ratio.
    const long total = (long)a->getWidth() * (long)a->getHeight();
    const long diff  = (long)diffcoords[2] * (long)diffcoords[3];
    if (total <= 0) return 0.0;
    double approx = 1.0 - (double)diff / (double)total;
    if (approx < 0.0) approx = 0.0;
    if (approx > 1.0) approx = 1.0;
    return approx;
  }

  // Manual scan with sampling and optional early-out.
  const LICE_pixel* p1 = a->getBits();
  const LICE_pixel* p2 = b->getBits();
  int span1 = a->getRowSpan();
  int span2 = b->getRowSpan();
  const int w = a->getWidth();

  if (a->isFlipped()) { p1 += span1 * (a->getHeight()-1); span1 = -span1; }
  if (b->isFlipped()) { p2 += span2 * (b->getHeight()-1); span2 = -span2; }

  const int sX = (cfg->sample_step_x > 0 ? cfg->sample_step_x : 1);
  const int sY = (cfg->sample_step_y > 0 ? cfg->sample_step_y : 1);

  // Compute total samples ahead of time to enable an early-out check.
  const int roi_w_s = (rw + (sX - 1)) / sX;
  const int roi_h_s = (rh + (sY - 1)) / sY;
  const long total_samples = (long)roi_w_s * (long)roi_h_s;
  if (total_samples <= 0) return 1.0;

  long equal_count = 0;
  long processed = 0;

  for (int yy = r.top; yy < r.bottom; yy += sY)
  {
    const LICE_pixel* row1 = p1 + yy * span1;
    const LICE_pixel* row2 = p2 + yy * span2;

    for (int xx = r.left; xx < r.right; xx += sX)
    {
      const LICE_pixel a_px = row1[xx];
      const LICE_pixel b_px = row2[xx];
      if (pixels_equal(a_px, b_px, cfg))
        ++equal_count;

      ++processed;

      if (cfg->enable_early_out)
      {
        // If even in the best case the threshold is unattainable, exit early.
        const long remaining = total_samples - processed;
        const long best_case_equal = equal_count + remaining;
        const double best_case_sim = (double)best_case_equal / (double)total_samples;
        if (best_case_sim < cfg->similarity_threshold)
          goto CALC_FINISH;
      }
    }
  }

CALC_FINISH:
  if (processed <= 0) return 1.0;
  double sim = (double)equal_count / (double)total_samples;
  if (sim < 0.0) sim = 0.0;
  if (sim > 1.0) sim = 1.0;
  return sim;
}

bool IsDuplicateFrame(const FrameInfo* prev,
                      const FrameInfo* curr,
                      const DuplicateFrameRemovalSettings* cfg,
                      double* out_similarity)
{
  if (!prev->bmp || !curr->bmp) {
    if (out_similarity) *out_similarity = 0.0;
    return false;
  }

  RECT roi;
  if (curr->w > 0 && curr->h > 0)
  {
    roi.left = curr->x; roi.top = curr->y; roi.right = curr->x + curr->w; roi.bottom = curr->y + curr->h;
  }
  else if (prev->w > 0 && prev->h > 0)
  {
    roi.left = prev->x; roi.top = prev->y; roi.right = prev->x + prev->w; roi.bottom = prev->y + prev->h;
  }
  else
  {
    roi.left = roi.top = 0;
    roi.right = (prev->bmp->getWidth() < curr->bmp->getWidth()) ? prev->bmp->getWidth() : curr->bmp->getWidth();
    roi.bottom = (prev->bmp->getHeight() < curr->bmp->getHeight()) ? prev->bmp->getHeight() : curr->bmp->getHeight();
  }

  double sim = CalculateSimilarity(prev->bmp, curr->bmp, &roi, cfg);
  if (out_similarity) *out_similarity = sim;
  return sim >= cfg->similarity_threshold;
}

// Collapses consecutive duplicates according to cfg->
size_t RemoveDuplicateFrames(const FrameInfo* input,
                             size_t input_count,
                             FrameArray* output,
                             const DuplicateFrameRemovalSettings* cfg,
                             IndexArray* removed_indices)
{
  if (!input || input_count == 0 || !cfg) return 0;

  // Initialize output array
  if (output) {
    FrameArray_Init(output, input_count / 2); // start with half the input capacity
  }

  // Initialize removed indices array if provided
  if (removed_indices) {
    IndexArray_Init(removed_indices, input_count / 4); // start with quarter capacity
  }

  // Group duplicates relative to the last kept frame.
  FrameInfo pending = input[0];
  int group_count = 1;
  int group_delay_sum = pending.delay_ms;
  size_t removed = 0;

  for (size_t i = 1; i < input_count; ++i)
  {
    const FrameInfo* cur = &input[i];
    double sim = 0.0;
    const bool is_dup = IsDuplicateFrame(pending, *cur, *cfg, &sim);

    if (is_dup)
    {
      // Extend the duplicate run.
      ++group_count;
      group_delay_sum += cur->delay_ms;

      if (cfg->keep_mode == kDuplicateKeepFirst)
      {
        // Keep the first; mark current as removed.
        ++removed;
        if (removed_indices) IndexArray_Add(removed_indices, i);
        continue;
      }
      else // keep last
      {
        // Replace pending with current, but keep accumulating stats.
        // We do not emit output yet.
        pending = *cur;
        ++removed; // The prior pending will be dropped in favor of the last.
        if (removed_indices) IndexArray_Add(removed_indices, i - 1);
        continue;
      }
    }

    // Flush the previous run (singleton or duplicates).
    if (cfg->delay_adjust_mode == kSum)
    {
      pending.delay_ms = group_delay_sum;
    }
    else if (cfg->delay_adjust_mode == kAverage && group_count > 0)
    {
      pending.delay_ms = group_delay_sum / group_count;
    }
    // else kDontAdjust keeps pending.delay_ms as set.

    if (output) FrameArray_Add(output, &pending);

    // Start new run from current
    pending = *cur;
    group_count = 1;
    group_delay_sum = cur->delay_ms;
  }

  // Flush the final run.
  if (cfg->delay_adjust_mode == kSum)
  {
    pending.delay_ms = group_delay_sum;
  }
  else if (cfg->delay_adjust_mode == kAverage && group_count > 0)
  {
    pending.delay_ms = group_delay_sum / group_count;
  }
  if (output) FrameArray_Add(output, &pending);

  return removed;
}
