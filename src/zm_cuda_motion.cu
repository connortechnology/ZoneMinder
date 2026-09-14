//
// ZoneMinder CUDA motion detection, Copyright (C) 2026 ZoneMinder
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//

#include "zm_cuda_motion.h"

#if HAVE_CUDA

#include <cuda.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <string>

// This file deliberately includes no ZoneMinder headers beyond its own: nvcc
// compiles it, and the logging and ffmpeg headers are not worth putting through
// a second compiler. Failures come back as false plus LastError().

namespace zm {
namespace cuda {

namespace {

constexpr uint8_t kWhite = 255;
constexpr uint8_t kBlack = 0;

// Caps on the component pass. A frame that needs more than this many
// propagation rounds or produces more blobs than this is reported truncated
// rather than silently wrong.
constexpr int kMaxLabelIterations = 512;
constexpr uint32_t kMaxBlobs = 4096;
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

dim3 Grid2D(int width, int height, dim3 block) {
  return dim3((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
}

__global__ void DeltaKernel(const uint8_t *__restrict__ cur, size_t cur_pitch,
                            const uint8_t *__restrict__ ref, size_t ref_pitch,
                            uint8_t *__restrict__ delta, size_t delta_pitch,
                            int width, int height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const int c = cur[y * cur_pitch + x];
  const int r = ref[y * ref_pitch + x];
  delta[y * delta_pitch + x] = static_cast<uint8_t>(abs(c - r));
}

// The alarmed test from alarmedpixels_row, pixel for pixel: inside the polygon,
// strictly above the minimum, at or below the maximum.
__global__ void ThresholdKernel(const uint8_t *__restrict__ delta, size_t delta_pitch,
                                const uint8_t *__restrict__ poly, size_t poly_pitch,
                                uint8_t *__restrict__ mask, size_t mask_pitch,
                                int width, int lo_y, int hi_y,
                                uint8_t min_threshold, uint8_t max_threshold,
                                uint32_t *__restrict__ out_count,
                                unsigned long long *__restrict__ out_sum) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = lo_y + blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y > hi_y) return;

  const uint8_t d = delta[y * delta_pitch + x];
  const uint8_t p = poly[y * poly_pitch + x];
  const bool alarmed = (p != 0) && (d > min_threshold) && (d <= max_threshold);
  mask[y * mask_pitch + x] = alarmed ? kWhite : kBlack;

  // One atomic per warp rather than per alarmed pixel: at 4K with a lively
  // scene the per-pixel version is most of the kernel's time.
  const unsigned int active = __ballot_sync(0xFFFFFFFFu, alarmed);
  const unsigned int lane = threadIdx.x & 31u;
  unsigned int sum = alarmed ? d : 0u;
  for (int offset = 16; offset > 0; offset >>= 1)
    sum += __shfl_down_sync(0xFFFFFFFFu, sum, offset);
  if (lane == 0 && active) {
    atomicAdd(out_count, __popc(active));
    atomicAdd(out_sum, static_cast<unsigned long long>(sum));
  }
}

// The filter stage of Zone::CheckAlarms: a white pixel survives when it takes
// part in at least one wholly white bx-by block that stays inside the zone.
//
// One difference from the CPU version, deliberate: that one edits the mask in
// place as it scans, so a pixel it has already cleared can make a later pixel
// fail its block test, and the result depends on scan order. This reads the
// input mask and writes a separate output, which is the order-independent
// reading of the same rule.
__global__ void FilterKernel(const uint8_t *__restrict__ mask_in, size_t in_pitch,
                             uint8_t *__restrict__ mask_out, size_t out_pitch,
                             const int *__restrict__ row_lo_x,
                             const int *__restrict__ row_hi_x,
                             int width, int lo_y, int hi_y,
                             int box_x, int box_y,
                             uint32_t *__restrict__ out_count) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = lo_y + blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y > hi_y) return;

  bool keep = false;
  if (mask_in[y * in_pitch + x] == kWhite) {
    const int lo_x = row_lo_x[y];
    const int hi_x = row_hi_x[y];
    if (lo_x >= 0 && x >= lo_x && x <= hi_x) {
      const int bx1 = box_x - 1;
      const int by1 = box_y - 1;
      const int ldx = (x >= (lo_x + bx1)) ? -bx1 : lo_x - x;
      const int hdx = (x <= (hi_x - bx1)) ? 0 : ((hi_x - x) - bx1);
      const int ldy = (y >= (lo_y + by1)) ? -by1 : lo_y - y;
      const int hdy = (y <= (hi_y - by1)) ? 0 : ((hi_y - y) - by1);

      for (int dy = ldy; !keep && dy <= hdy; dy++) {
        for (int dx = ldx; !keep && dx <= hdx; dx++) {
          bool block = true;
          for (int dy2 = 0; block && dy2 < box_y; dy2++) {
            for (int dx2 = 0; block && dx2 < box_x; dx2++) {
              if (!mask_in[(y + dy + dy2) * in_pitch + (x + dx + dx2)]) block = false;
            }
          }
          if (block) keep = true;
        }
      }
    }
  }

  mask_out[y * out_pitch + x] = keep ? kWhite : kBlack;

  const unsigned int active = __ballot_sync(0xFFFFFFFFu, keep);
  if ((threadIdx.x & 31u) == 0 && active) atomicAdd(out_count, __popc(active));
}

// Components are 4-connected, matching the CPU pass, which only ever looks at
// the pixel to the left and the one above.
__global__ void LabelInitKernel(const uint8_t *__restrict__ mask, size_t mask_pitch,
                                uint32_t *__restrict__ labels,
                                int width, int lo_y, int hi_y) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = lo_y + blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y > hi_y) return;

  const uint32_t idx = static_cast<uint32_t>(y) * width + x;
  labels[idx] = mask[y * mask_pitch + x] ? idx : kNoSlot;
}

// Each round every pixel takes the smallest label among itself and its alarmed
// neighbours, so a component converges on the lowest index it contains.
__global__ void LabelPropagateKernel(uint32_t *__restrict__ labels,
                                     int width, int lo_y, int hi_y,
                                     int *__restrict__ changed) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = lo_y + blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y > hi_y) return;

  const uint32_t idx = static_cast<uint32_t>(y) * width + x;
  uint32_t label = labels[idx];
  if (label == kNoSlot) return;

  uint32_t best = label;
  if (x > 0) best = min(best, labels[idx - 1]);
  if (x < width - 1) best = min(best, labels[idx + 1]);
  if (y > lo_y) best = min(best, labels[idx - width]);
  if (y < hi_y) best = min(best, labels[idx + width]);

  if (best < label) {
    labels[idx] = best;
    *changed = 1;
  }
}

__global__ void BlobSlotKernel(const uint32_t *__restrict__ labels,
                               uint32_t *__restrict__ slot_map,
                               int width, int lo_y, int hi_y,
                               uint32_t *__restrict__ blob_count) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = lo_y + blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y > hi_y) return;

  const uint32_t idx = static_cast<uint32_t>(y) * width + x;
  // The pixel whose label is its own index is the component's canonical one,
  // so exactly one thread per component claims a slot.
  if (labels[idx] == idx) {
    const uint32_t slot = atomicAdd(blob_count, 1u);
    if (slot < kMaxBlobs) slot_map[idx] = slot;
  }
}

__global__ void BlobStatsKernel(const uint32_t *__restrict__ labels,
                                const uint32_t *__restrict__ slot_map,
                                int width, int lo_y, int hi_y,
                                uint32_t *__restrict__ counts,
                                int *__restrict__ lo_xs, int *__restrict__ hi_xs,
                                int *__restrict__ lo_ys, int *__restrict__ hi_ys,
                                unsigned long long *__restrict__ x_sums,
                                unsigned long long *__restrict__ y_sums) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = lo_y + blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y > hi_y) return;

  const uint32_t idx = static_cast<uint32_t>(y) * width + x;
  const uint32_t label = labels[idx];
  if (label == kNoSlot) return;

  const uint32_t slot = slot_map[label];
  if (slot >= kMaxBlobs) return;

  atomicAdd(&counts[slot], 1u);
  atomicMin(&lo_xs[slot], x);
  atomicMax(&hi_xs[slot], x);
  atomicMin(&lo_ys[slot], y);
  atomicMax(&hi_ys[slot], y);
  // Coordinate totals so a weighted alarm centre (ZM_WEIGHTED_ALARM_CENTRES)
  // can be worked out without bringing the mask back to the host.
  atomicAdd(&x_sums[slot], static_cast<unsigned long long>(x));
  atomicAdd(&y_sums[slot], static_cast<unsigned long long>(y));
}

// Inactive zones blank their area of the delta so no other zone sees motion
// there, which the CPU path does with Image::Fill before any zone is checked.
__global__ void MaskOutKernel(uint8_t *__restrict__ delta, size_t delta_pitch,
                              const uint8_t *__restrict__ poly, size_t poly_pitch,
                              int width, int lo_y, int hi_y) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = lo_y + blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y > hi_y) return;

  if (poly[y * poly_pitch + x]) delta[y * delta_pitch + x] = 0;
}

// fast_blend quantises the percentage to a power-of-two shift and works in
// integers; std_blend keeps it in double. Which one a CPU monitor uses is
// ZM_FAST_IMAGE_BLENDS, so both live here and the caller picks.
__global__ void BlendFastKernel(uint8_t *__restrict__ ref, size_t ref_pitch,
                                const uint8_t *__restrict__ cur, size_t cur_pitch,
                                int width, int height, int divider) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const int r = ref[y * ref_pitch + x];
  const int c = cur[y * cur_pitch + x];
  ref[y * ref_pitch + x] = static_cast<uint8_t>(r + ((c - r) >> divider));
}

__global__ void BlendKernel(uint8_t *__restrict__ ref, size_t ref_pitch,
                            const uint8_t *__restrict__ cur, size_t cur_pitch,
                            int width, int height, double divide, double opacity) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;

  const double r = ref[y * ref_pitch + x];
  const double c = cur[y * cur_pitch + x];
  ref[y * ref_pitch + x] = static_cast<uint8_t>(r * opacity + c * divide);
}

// std_fastblend's percentage-to-shift table, so the device blend lands on the
// same value the CPU one would have.
int FastBlendDivider(double percent) {
  if (percent < 2.34375) return 6;
  if (percent < 4.6875) return 5;
  if (percent < 9.375) return 4;
  if (percent < 18.75) return 3;
  if (percent < 37.5) return 2;
  return 1;
}

}  // namespace

class MotionDetector::Impl {
 public:
  struct Zone {
    uint8_t *mask = nullptr;        // polygon, pitched
    size_t mask_pitch = 0;
    uint8_t *alarm = nullptr;       // thresholded output, pitched
    size_t alarm_pitch = 0;
    uint8_t *filtered = nullptr;    // filter output, pitched, only when wanted
    size_t filtered_pitch = 0;
    int *row_lo_x = nullptr;
    int *row_hi_x = nullptr;
    int lo_y = 0;
    int hi_y = 0;
    uint8_t min_threshold = 0;
    uint8_t max_threshold = 255;
    int filter_box_x = 1;
    int filter_box_y = 1;
    bool want_filter = false;
    bool want_blobs = false;
    bool inactive = false;
  };

  ~Impl() { Release(); }

  void Release() {
    for (Zone &zone : zones) {
      cudaFree(zone.mask);
      cudaFree(zone.alarm);
      cudaFree(zone.filtered);
      cudaFree(zone.row_lo_x);
      cudaFree(zone.row_hi_x);
    }
    zones.clear();
    cudaFree(ref);
    cudaFree(delta);
    cudaFree(labels);
    cudaFree(slot_map);
    cudaFree(scratch);
    ref = delta = nullptr;
    labels = slot_map = nullptr;
    scratch = nullptr;
  }

  CUcontext context = nullptr;
  uint8_t *ref = nullptr;
  size_t ref_pitch = 0;
  uint8_t *delta = nullptr;
  size_t delta_pitch = 0;
  uint32_t *labels = nullptr;
  uint32_t *slot_map = nullptr;
  // One allocation for every small per-frame value the kernels write: the
  // counters, the changed flag and the per-blob arrays.
  void *scratch = nullptr;
  std::vector<Zone> zones;
  std::string last_error;
};

namespace {

// Makes the decoder's context current for the length of a call. ffmpeg owns the
// context; we borrow it and always hand it back.
class ContextGuard {
 public:
  explicit ContextGuard(CUcontext ctx) : pushed_(false) {
    if (ctx) pushed_ = (cuCtxPushCurrent(ctx) == CUDA_SUCCESS);
  }
  ~ContextGuard() {
    if (pushed_) {
      CUcontext popped = nullptr;
      cuCtxPopCurrent(&popped);
    }
  }

 private:
  bool pushed_;
};

}  // namespace

MotionDetector::MotionDetector() : impl_(new Impl()) {}

MotionDetector::~MotionDetector() {
  delete impl_;
}

const char *MotionDetector::LastError() const {
  return impl_->last_error.c_str();
}

bool MotionDetector::Available() {
  int count = 0;
  return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

bool MotionDetector::Init(int width, int height, void *cuda_context) {
  if (width <= 0 || height <= 0) {
    impl_->last_error = "Invalid dimensions for CUDA motion detection";
    return false;
  }

  impl_->Release();
  impl_->context = static_cast<CUcontext>(cuda_context);
  width_ = width;
  height_ = height;
  have_reference_ = false;

  ContextGuard guard(impl_->context);

  cudaError_t err = cudaMallocPitch(reinterpret_cast<void **>(&impl_->ref),
                                    &impl_->ref_pitch, width, height);
  if (err == cudaSuccess)
    err = cudaMallocPitch(reinterpret_cast<void **>(&impl_->delta),
                          &impl_->delta_pitch, width, height);
  if (err != cudaSuccess) {
    impl_->last_error = std::string("Failed to allocate device planes: ") + cudaGetErrorString(err);
    return false;
  }

  // Counters (alarm count, diff sum, filter count, blob count, changed flag)
  // followed by the per-blob arrays.
  const size_t scratch_bytes = sizeof(unsigned long long) * 8
      + kMaxBlobs * (sizeof(uint32_t) + 4 * sizeof(int) + 2 * sizeof(unsigned long long));
  err = cudaMalloc(&impl_->scratch, scratch_bytes);
  if (err != cudaSuccess) {
    impl_->last_error = std::string("Failed to allocate device scratch: ") + cudaGetErrorString(err);
    return false;
  }
  return true;
}

bool MotionDetector::SetZones(const std::vector<ZoneSpec> &specs) {
  ContextGuard guard(impl_->context);

  for (Impl::Zone &zone : impl_->zones) {
    cudaFree(zone.mask);
    cudaFree(zone.alarm);
    cudaFree(zone.filtered);
    cudaFree(zone.row_lo_x);
    cudaFree(zone.row_hi_x);
  }
  impl_->zones.clear();
  impl_->zones.resize(specs.size());

  bool wants_blobs = false;

  for (size_t i = 0; i < specs.size(); i++) {
    const ZoneSpec &spec = specs[i];
    Impl::Zone &zone = impl_->zones[i];

    zone.lo_y = std::max(0, spec.lo_y);
    zone.hi_y = std::min(height_ - 1, spec.hi_y);
    zone.min_threshold = spec.min_pixel_threshold;
    zone.max_threshold = spec.max_pixel_threshold ? spec.max_pixel_threshold : 255;
    zone.filter_box_x = spec.filter_box_x;
    zone.filter_box_y = spec.filter_box_y;
    // A 1x1 box keeps every pixel, which is what the CPU path short-circuits.
    zone.want_filter = spec.want_filter && (spec.filter_box_x > 1 || spec.filter_box_y > 1);
    zone.want_blobs = spec.want_blobs;
    zone.inactive = spec.inactive;
    wants_blobs = wants_blobs || spec.want_blobs;

    cudaError_t err = cudaMallocPitch(reinterpret_cast<void **>(&zone.mask),
                                      &zone.mask_pitch, width_, height_);
    if (err == cudaSuccess)
      err = cudaMallocPitch(reinterpret_cast<void **>(&zone.alarm),
                            &zone.alarm_pitch, width_, height_);
    if (err == cudaSuccess && zone.want_filter)
      err = cudaMallocPitch(reinterpret_cast<void **>(&zone.filtered),
                            &zone.filtered_pitch, width_, height_);
    if (err == cudaSuccess)
      err = cudaMalloc(reinterpret_cast<void **>(&zone.row_lo_x), height_ * sizeof(int));
    if (err == cudaSuccess)
      err = cudaMalloc(reinterpret_cast<void **>(&zone.row_hi_x), height_ * sizeof(int));
    if (err != cudaSuccess) {
      impl_->last_error = std::string("Failed to allocate zone buffers: ") + cudaGetErrorString(err);
      return false;
    }

    // The mask is zero outside the polygon, so the whole plane has to be
    // cleared even though only the bounding box rows get written.
    err = cudaMemset2D(zone.mask, zone.mask_pitch, 0, width_, height_);
    if (err == cudaSuccess)
      err = cudaMemset2D(zone.alarm, zone.alarm_pitch, 0, width_, height_);
    if (err == cudaSuccess && zone.filtered)
      err = cudaMemset2D(zone.filtered, zone.filtered_pitch, 0, width_, height_);
    if (err == cudaSuccess && spec.mask)
      err = cudaMemcpy2D(zone.mask, zone.mask_pitch, spec.mask, width_,
                         width_, height_, cudaMemcpyHostToDevice);
    if (err == cudaSuccess && spec.row_lo_x)
      err = cudaMemcpy(zone.row_lo_x, spec.row_lo_x, height_ * sizeof(int), cudaMemcpyHostToDevice);
    if (err == cudaSuccess && spec.row_hi_x)
      err = cudaMemcpy(zone.row_hi_x, spec.row_hi_x, height_ * sizeof(int), cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
      impl_->last_error = std::string("Failed to upload zone: ") + cudaGetErrorString(err);
      return false;
    }
  }

  if (wants_blobs && !impl_->labels) {
    const size_t pixels = static_cast<size_t>(width_) * height_;
    cudaError_t err = cudaMalloc(reinterpret_cast<void **>(&impl_->labels), pixels * sizeof(uint32_t));
    if (err == cudaSuccess)
      err = cudaMalloc(reinterpret_cast<void **>(&impl_->slot_map), pixels * sizeof(uint32_t));
    if (err != cudaSuccess) {
      impl_->last_error = std::string("Failed to allocate label buffers: ") + cudaGetErrorString(err);
      return false;
    }
  }
  return true;
}

bool MotionDetector::AssignReference(const uint8_t *y_plane, size_t pitch) {
  ContextGuard guard(impl_->context);

  cudaError_t err = cudaMemcpy2D(impl_->ref, impl_->ref_pitch, y_plane, pitch,
                                 width_, height_, cudaMemcpyDefault);
  if (err != cudaSuccess) {
    impl_->last_error = std::string("Failed to seed the reference plane: ") + cudaGetErrorString(err);
    return false;
  }
  have_reference_ = true;
  return true;
}

bool MotionDetector::Detect(const uint8_t *y_plane, size_t pitch, std::vector<ZoneResult> &results) {
  if (!have_reference_) {
    impl_->last_error = "Detect called before the reference plane was seeded";
    return false;
  }

  ContextGuard guard(impl_->context);

  const dim3 block(32, 8);
  DeltaKernel<<<Grid2D(width_, height_, block), block>>>(
      y_plane, pitch, impl_->ref, impl_->ref_pitch,
      impl_->delta, impl_->delta_pitch, width_, height_);

  results.assign(impl_->zones.size(), ZoneResult());

  // Inactive zones first: they blank their area of the delta so no zone checked
  // below sees motion there, which is what Image::Fill does on the CPU path.
  for (const Impl::Zone &zone : impl_->zones) {
    if (!zone.inactive || zone.hi_y < zone.lo_y) continue;
    const dim3 zone_grid = Grid2D(width_, zone.hi_y - zone.lo_y + 1, block);
    MaskOutKernel<<<zone_grid, block>>>(impl_->delta, impl_->delta_pitch,
                                        zone.mask, zone.mask_pitch,
                                        width_, zone.lo_y, zone.hi_y);
  }

  // Counters live at the front of the scratch allocation; the blob arrays
  // follow it.
  unsigned long long *counters = static_cast<unsigned long long *>(impl_->scratch);
  uint32_t *alarm_count = reinterpret_cast<uint32_t *>(counters);
  unsigned long long *diff_sum = counters + 1;
  uint32_t *filter_count = reinterpret_cast<uint32_t *>(counters + 2);
  uint32_t *blob_count = reinterpret_cast<uint32_t *>(counters + 3);
  int *changed = reinterpret_cast<int *>(counters + 4);
  uint32_t *blob_counts = reinterpret_cast<uint32_t *>(counters + 8);
  int *blob_lo_x = reinterpret_cast<int *>(blob_counts + kMaxBlobs);
  int *blob_hi_x = blob_lo_x + kMaxBlobs;
  int *blob_lo_y = blob_hi_x + kMaxBlobs;
  int *blob_hi_y = blob_lo_y + kMaxBlobs;
  unsigned long long *blob_x_sum = reinterpret_cast<unsigned long long *>(blob_hi_y + kMaxBlobs);
  unsigned long long *blob_y_sum = blob_x_sum + kMaxBlobs;

  for (size_t i = 0; i < impl_->zones.size(); i++) {
    Impl::Zone &zone = impl_->zones[i];
    ZoneResult &result = results[i];
    if (zone.inactive || zone.hi_y < zone.lo_y) continue;

    const int rows = zone.hi_y - zone.lo_y + 1;
    const dim3 zone_grid = Grid2D(width_, rows, block);

    cudaMemset(counters, 0, sizeof(unsigned long long) * 8);
    ThresholdKernel<<<zone_grid, block>>>(
        impl_->delta, impl_->delta_pitch, zone.mask, zone.mask_pitch,
        zone.alarm, zone.alarm_pitch, width_, zone.lo_y, zone.hi_y,
        zone.min_threshold, zone.max_threshold, alarm_count, diff_sum);

    const uint8_t *stage = zone.alarm;
    size_t stage_pitch = zone.alarm_pitch;

    if (zone.want_filter) {
      FilterKernel<<<zone_grid, block>>>(
          zone.alarm, zone.alarm_pitch, zone.filtered, zone.filtered_pitch,
          zone.row_lo_x, zone.row_hi_x, width_, zone.lo_y, zone.hi_y,
          zone.filter_box_x, zone.filter_box_y, filter_count);
      stage = zone.filtered;
      stage_pitch = zone.filtered_pitch;
    }

    uint32_t host_blob_count = 0;
    if (zone.want_blobs) {
      LabelInitKernel<<<zone_grid, block>>>(stage, stage_pitch, impl_->labels,
                                            width_, zone.lo_y, zone.hi_y);
      int iterations = 0;
      for (; iterations < kMaxLabelIterations; iterations++) {
        cudaMemset(changed, 0, sizeof(int));
        LabelPropagateKernel<<<zone_grid, block>>>(impl_->labels, width_, zone.lo_y, zone.hi_y, changed);
        int host_changed = 0;
        cudaMemcpy(&host_changed, changed, sizeof(int), cudaMemcpyDeviceToHost);
        if (!host_changed) break;
      }
      if (iterations >= kMaxLabelIterations) result.blobs_truncated = true;

      const size_t pixels = static_cast<size_t>(width_) * height_;
      cudaMemset(impl_->slot_map, 0xFF, pixels * sizeof(uint32_t));
      cudaMemset(blob_counts, 0, kMaxBlobs * sizeof(uint32_t));
      // Bounding boxes start inverted so atomicMin/atomicMax build them up.
      cudaMemset(blob_lo_x, 0x7F, kMaxBlobs * sizeof(int));
      cudaMemset(blob_lo_y, 0x7F, kMaxBlobs * sizeof(int));
      cudaMemset(blob_hi_x, 0x80, kMaxBlobs * sizeof(int));
      cudaMemset(blob_hi_y, 0x80, kMaxBlobs * sizeof(int));
      cudaMemset(blob_x_sum, 0, kMaxBlobs * sizeof(unsigned long long));
      cudaMemset(blob_y_sum, 0, kMaxBlobs * sizeof(unsigned long long));

      BlobSlotKernel<<<zone_grid, block>>>(impl_->labels, impl_->slot_map,
                                           width_, zone.lo_y, zone.hi_y, blob_count);
      BlobStatsKernel<<<zone_grid, block>>>(impl_->labels, impl_->slot_map,
                                            width_, zone.lo_y, zone.hi_y,
                                            blob_counts, blob_lo_x, blob_hi_x,
                                            blob_lo_y, blob_hi_y,
                                            blob_x_sum, blob_y_sum);
      cudaMemcpy(&host_blob_count, blob_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);
    }

    uint32_t host_counters[4] = {0, 0, 0, 0};
    unsigned long long host_sum = 0;
    cudaMemcpy(host_counters, alarm_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);
    cudaMemcpy(&host_sum, diff_sum, sizeof(unsigned long long), cudaMemcpyDeviceToHost);
    cudaMemcpy(&host_counters[2], filter_count, sizeof(uint32_t), cudaMemcpyDeviceToHost);

    result.alarm_pixels = host_counters[0];
    result.pixel_diff_sum = host_sum;
    result.filter_pixels = zone.want_filter ? host_counters[2] : host_counters[0];

    if (zone.want_blobs && host_blob_count) {
      if (host_blob_count > kMaxBlobs) {
        result.blobs_truncated = true;
        host_blob_count = kMaxBlobs;
      }
      std::vector<uint32_t> counts(host_blob_count);
      std::vector<int> lo_x(host_blob_count), hi_x(host_blob_count);
      std::vector<int> lo_y(host_blob_count), hi_y(host_blob_count);
      std::vector<unsigned long long> x_sum(host_blob_count), y_sum(host_blob_count);
      cudaMemcpy(counts.data(), blob_counts, host_blob_count * sizeof(uint32_t), cudaMemcpyDeviceToHost);
      cudaMemcpy(lo_x.data(), blob_lo_x, host_blob_count * sizeof(int), cudaMemcpyDeviceToHost);
      cudaMemcpy(hi_x.data(), blob_hi_x, host_blob_count * sizeof(int), cudaMemcpyDeviceToHost);
      cudaMemcpy(lo_y.data(), blob_lo_y, host_blob_count * sizeof(int), cudaMemcpyDeviceToHost);
      cudaMemcpy(hi_y.data(), blob_hi_y, host_blob_count * sizeof(int), cudaMemcpyDeviceToHost);
      cudaMemcpy(x_sum.data(), blob_x_sum, host_blob_count * sizeof(unsigned long long), cudaMemcpyDeviceToHost);
      cudaMemcpy(y_sum.data(), blob_y_sum, host_blob_count * sizeof(unsigned long long), cudaMemcpyDeviceToHost);

      result.blobs.reserve(host_blob_count);
      for (uint32_t b = 0; b < host_blob_count; b++) {
        if (!counts[b]) continue;
        Blob blob;
        blob.count = counts[b];
        blob.lo_x = lo_x[b];
        blob.hi_x = hi_x[b];
        blob.lo_y = lo_y[b];
        blob.hi_y = hi_y[b];
        blob.x_sum = x_sum[b];
        blob.y_sum = y_sum[b];
        result.blobs.push_back(blob);
      }
    }
  }

  cudaError_t err = cudaDeviceSynchronize();
  if (err != cudaSuccess) {
    impl_->last_error = std::string("CUDA motion detection failed: ") + cudaGetErrorString(err);
    return false;
  }
  return true;
}

bool MotionDetector::BlendReference(const uint8_t *y_plane, size_t pitch, double percent, bool fast) {
  if (percent <= 0) return true;

  ContextGuard guard(impl_->context);

  const dim3 block(32, 8);
  const dim3 grid = Grid2D(width_, height_, block);
  if (fast) {
    BlendFastKernel<<<grid, block>>>(impl_->ref, impl_->ref_pitch, y_plane, pitch,
                                     width_, height_, FastBlendDivider(percent));
  } else {
    const double divide = percent / 100.0;
    BlendKernel<<<grid, block>>>(impl_->ref, impl_->ref_pitch, y_plane, pitch,
                                 width_, height_, divide, 1.0 - divide);
  }

  cudaError_t err = cudaDeviceSynchronize();
  if (err != cudaSuccess) {
    impl_->last_error = std::string("CUDA blend failed: ") + cudaGetErrorString(err);
    return false;
  }
  return true;
}

bool MotionDetector::DownloadMask(size_t zone_index, uint8_t *dest, size_t dest_stride) const {
  if (zone_index >= impl_->zones.size()) {
    impl_->last_error = "DownloadMask called with an out of range zone";
    return false;
  }

  ContextGuard guard(impl_->context);

  const Impl::Zone &zone = impl_->zones[zone_index];
  const uint8_t *src = zone.want_filter ? zone.filtered : zone.alarm;
  const size_t src_pitch = zone.want_filter ? zone.filtered_pitch : zone.alarm_pitch;
  cudaError_t err = cudaMemcpy2D(dest, dest_stride, src, src_pitch,
                                 width_, height_, cudaMemcpyDeviceToHost);
  if (err != cudaSuccess) {
    impl_->last_error = std::string("Failed to download the alarm mask: ") + cudaGetErrorString(err);
    return false;
  }
  return true;
}

bool MotionDetector::DownloadReference(uint8_t *dest, size_t dest_stride) const {
  ContextGuard guard(impl_->context);

  cudaError_t err = cudaMemcpy2D(dest, dest_stride, impl_->ref, impl_->ref_pitch,
                                 width_, height_, cudaMemcpyDeviceToHost);
  if (err != cudaSuccess) {
    impl_->last_error = std::string("Failed to download the reference plane: ") + cudaGetErrorString(err);
    return false;
  }
  return true;
}

}  // namespace cuda
}  // namespace zm

#endif  // HAVE_CUDA
