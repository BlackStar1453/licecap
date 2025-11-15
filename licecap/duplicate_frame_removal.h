// duplicate_frame_removal.h
//
// Lightweight utilities for detecting and removing duplicate frames
// in licecap, using LICE bitmaps and project conventions.
//
// This header defines the data structures and configuration used for
// duplicate-frame removal, and the public APIs for similarity
// evaluation and in-sequence duplicate collapsing.
//
// Notes:
// - All functions operate on LICE_IBitmap instances; no ownership is
//   taken and no bitmaps are freed. Callers retain ownership.
// - Similarity is computed via pixel-level comparisons with optional
//   channel masking and sampling for performance.
// - Removal works on consecutive duplicates only (temporal neighbors),
//   which matches licecap's WM_TIMER capture flow.

#ifndef DUPLICATE_FRAME_REMOVAL_H_
#define DUPLICATE_FRAME_REMOVAL_H_

#include <stddef.h>
#include <vector>

#include "../WDL/lice/lice.h"  // LICE_IBitmap, LICE_pixel and helpers

// FrameInfo describes a captured frame and basic timing/geometry.
// - index: logical index in the capture sequence
// - bmp:   pointer to the LICE bitmap data for the frame
// - delay_ms: frame delay in milliseconds (as used for GIF writing)
// - x,y,w,h: optional region of interest (ROI) within the bitmap used for
//            comparisons (full frame if w<=0 or h<=0)
struct FrameInfo
{
  int index;
  LICE_IBitmap* bmp;
  int delay_ms;
  int x, y, w, h; // ROI for comparison; set w/h<=0 for full-frame

  FrameInfo()
    : index(0), bmp(NULL), delay_ms(0), x(0), y(0), w(0), h(0) {}

  FrameInfo(int idx, LICE_IBitmap* b, int delay)
    : index(idx), bmp(b), delay_ms(delay), x(0), y(0), w(0), h(0) {}
};

// Which frame in a duplicate run to keep
enum DuplicateRemovalMode
{
  kDuplicateKeepFirst = 0,
  kDuplicateKeepLast  = 1
};

// How to adjust delays when collapsing duplicates
enum DelayAdjustMode
{
  kDontAdjust = 0,  // leave kept frame delay as-is
  kAverage    = 1,  // average of delays across the duplicate run
  kSum        = 2   // sum delays across the duplicate run (typical)
};

// Settings for duplicate detection and removal
struct DuplicateFrameRemovalSettings
{
  // Ratio threshold in [0,1]. Frames with similarity >= threshold are
  // considered duplicates.
  double similarity_threshold;

  // Pixel sampling step to reduce cost. 1 = check every pixel. Higher
  // values subsample uniformly in X/Y (e.g. 2 checks every other pixel).
  int sample_step_x;
  int sample_step_y;

  // Per-channel absolute tolerance in [0,255]. When >0, two pixels are
  // considered equal if each channel difference <= tolerance (for the
  // channels enabled by channel_mask). When 0, equality is exact.
  int per_channel_tolerance;

  // Channel mask for equality checks (use LICE_RGBA(bitsR,bitsG,bitsB,bitsA)).
  // Any channel bit cleared in this mask is ignored in strict (tolerance==0)
  // comparisons. For tolerant comparisons (>0), this mask selects which
  // channels are examined.
  LICE_pixel channel_mask;

  // Removal policy and delay adjustment.
  DuplicateRemovalMode keep_mode;
  DelayAdjustMode delay_adjust_mode;

  // Optional early-out: if true, the similarity calculation can stop
  // early when it is impossible to reach the threshold.
  bool enable_early_out;

  DuplicateFrameRemovalSettings()
    : similarity_threshold(0.90),
      sample_step_x(1),
      sample_step_y(1),
      per_channel_tolerance(0),
      channel_mask(LICE_RGBA(255,255,255,0)), // ignore alpha by default
      keep_mode(kDuplicateKeepFirst),
      delay_adjust_mode(kSum),
      enable_early_out(true)
  {}
};

// Calculate pixel-level similarity between two bitmaps.
// Returns a value in [0,1], where 1.0 means identical under settings.
// If roi is non-NULL, comparison is restricted to the given rectangle.
double CalculateSimilarity(LICE_IBitmap* a,
                           LICE_IBitmap* b,
                           const RECT* roi,
                           const DuplicateFrameRemovalSettings& cfg);

// Lightweight duplicate test for two frames. Returns true if similar
// enough under cfg. Optionally writes the computed similarity.
bool IsDuplicateFrame(const FrameInfo& prev,
                      const FrameInfo& curr,
                      const DuplicateFrameRemovalSettings& cfg,
                      double* out_similarity);

// Remove consecutive duplicates from an input sequence.
// - input:  original frames in capture order
// - output: filtered frames after duplicate removal
// - removed_indices: optional output of indices from the input sequence
//                    that were removed
// Returns the number of frames removed.
size_t RemoveDuplicateFrames(const std::vector<FrameInfo>& input,
                             std::vector<FrameInfo>& output,
                             const DuplicateFrameRemovalSettings& cfg,
                             std::vector<size_t>* removed_indices);

#endif // DUPLICATE_FRAME_REMOVAL_H_
