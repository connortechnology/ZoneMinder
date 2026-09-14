//
// ZoneMinder CUDA motion detection, Copyright (C) 2026 ZoneMinder
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//

#ifndef ZM_CUDA_MOTION_H
#define ZM_CUDA_MOTION_H

#include "zm_config.h"

#if HAVE_CUDA

#include <cstdint>
#include <vector>

//
// Device-side half of motion detection: the per-pixel work (delta, threshold,
// the box filter, connected components and the reference blend) runs on the
// card against the Y plane of a decoded CUDA frame, and only counts come back.
//
// Deliberately *not* here: every threshold, score and alarm-centre decision.
// Those stay in Zone::CheckAlarms so there is one copy of the policy and the
// CUDA path cannot drift away from what a CPU monitor scores. What this returns
// is the raw material that policy consumes -- alarmed pixel count, the sum of
// their differences, the surviving count after the filter, and one entry per
// connected component.
//
namespace zm {
namespace cuda {

// A zone as the kernels need it. Rebuilt only when the zone is reloaded.
struct ZoneSpec {
  // Polygon mask, width*height bytes, non-zero inside the polygon. Same buffer
  // Zone keeps as pg_image.
  const uint8_t *mask = nullptr;
  // Per-row polygon spans, height entries each. The filter reproduces the CPU
  // edge handling, which clamps against the row's own span rather than the
  // bounding box, so both are needed.
  const int *row_lo_x = nullptr;
  const int *row_hi_x = nullptr;
  int lo_y = 0;
  int hi_y = 0;

  uint8_t min_pixel_threshold = 0;
  // 0 means "no maximum", which the CPU path turns into 255.
  uint8_t max_pixel_threshold = 0;

  // 1x1 means no filtering. Only consulted when want_filter is set.
  int filter_box_x = 1;
  int filter_box_y = 1;

  bool want_filter = false;
  bool want_blobs = false;
};

struct Blob {
  uint32_t count = 0;
  int lo_x = 0;
  int lo_y = 0;
  int hi_x = 0;
  int hi_y = 0;
};

struct ZoneResult {
  uint32_t alarm_pixels = 0;
  // Sum of the delta values of the alarmed pixels. 64-bit because a 4K frame of
  // maximum differences overflows 32 bits.
  uint64_t pixel_diff_sum = 0;
  // Only meaningful when the spec asked for the filter.
  uint32_t filter_pixels = 0;
  // Raw components, before any min/max blob filtering: that is the caller's to
  // apply, exactly as the CPU path does.
  std::vector<Blob> blobs;
  // True when the component labelling hit its iteration or blob cap and the
  // blob list is therefore incomplete.
  bool blobs_truncated = false;
};

class MotionDetector {
 public:
  MotionDetector();
  ~MotionDetector();

  MotionDetector(const MotionDetector &) = delete;
  MotionDetector &operator=(const MotionDetector &) = delete;

  // True when a CUDA device is present and usable. Cheap after the first call.
  static bool Available();

  // cuda_context is the CUcontext ffmpeg decoded into, from the frame's
  // AVHWDeviceContext; it is made current around every launch. Pass nullptr to
  // use whatever context is already current, which is what the tests do.
  bool Init(int width, int height, void *cuda_context = nullptr);

  // Copies each zone's mask and row spans to the card. Call on zone reload.
  bool SetZones(const std::vector<ZoneSpec> &zones);

  // True once a reference frame exists; until then Detect() has nothing to
  // compare against and the caller should seed it with AssignReference().
  bool HasReference() const { return have_reference_; }

  // Seeds the reference from a device Y plane, for the first frame.
  bool AssignReference(const uint8_t *y_plane, size_t pitch);

  // Runs delta + per-zone threshold (+ filter, + components) for every zone set
  // by SetZones. results is resized to the zone count.
  bool Detect(const uint8_t *y_plane, size_t pitch, std::vector<ZoneResult> &results);

  // ref = ref + (cur - ref) * percent, matching whichever CPU blend the build
  // is configured for: fast_blend quantises percent to a power-of-two shift,
  // so pass fast=true to reproduce it exactly.
  bool BlendReference(const uint8_t *y_plane, size_t pitch, double percent, bool fast);

  // Copies a zone's alarm mask back to the host, for the analysis image
  // overlay. Only worth calling for a zone that actually alarmed.
  bool DownloadMask(size_t zone_index, uint8_t *dest, size_t dest_stride) const;

  // Reference plane as the card holds it, for diagnostics and tests.
  bool DownloadReference(uint8_t *dest, size_t dest_stride) const;

  int Width() const { return width_; }
  int Height() const { return height_; }

  // Why the last call returned false. Empty when nothing has failed. The .cu
  // stays clear of ZoneMinder's logging headers, so callers log this instead.
  const char *LastError() const;

 private:
  class Impl;
  Impl *impl_;
  int width_ = 0;
  int height_ = 0;
  bool have_reference_ = false;
};

}  // namespace cuda
}  // namespace zm

#endif  // HAVE_CUDA
#endif  // ZM_CUDA_MOTION_H
