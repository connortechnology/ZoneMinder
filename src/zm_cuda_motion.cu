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
#include <climits>
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

// A component converges on the lowest pixel index it contains. Doing that a
// neighbour at a time costs one round per pixel of a blob's diameter -- a 60
// pixel wide rectangle needed 60 rounds, each one a launch and a readback. A
// sweep carries the running minimum along a whole run instead, so a run settles
// in a single pass and a blob in a handful of them regardless of its size.
//
// A sweep is split into fixed length chunks rather than given a thread per
// line: a thread per line is 1080 threads for a 1080 row frame, which leaves a
// GPU almost idle and made these kernels 60% of the frame's device time. Each
// chunk is independent, so the work scales with pixels instead of lines; a
// label crossing a chunk boundary is picked up by the next round.
//
// Rows first, then columns; alternating the two is what lets an L or a diagonal
// staircase converge. Connectivity is unchanged: a sweep only ever carries a
// label across pixels that are adjacent and labelled, which is the same 4-way
// rule the CPU pass applies.
// The row direction wants a warp per row, not a thread per chunk: lanes read
// consecutive pixels, so a window of 32 is one memory transaction instead of
// 32 scattered ones. Chunking this kernel the way the column one is chunked
// made it slower for exactly that reason -- more threads, each its own
// transaction.
//
// Within a window the running minimum comes from a segmented scan: a segment
// breaks at every unlabelled pixel, so a label is only ever carried between
// pixels that are adjacent and labelled.
__device__ __forceinline__ uint32_t SegmentedMinScan(uint32_t value, bool valid,
                                                     unsigned int valid_mask,
                                                     int lane, uint32_t carry_in) {
  // The last unlabelled lane at or before this one ends the previous segment.
  const unsigned int upto = (lane == 31) ? 0xFFFFFFFFu : ((1u << (lane + 1)) - 1u);
  const unsigned int breaks = (~valid_mask) & upto;
  const int last_break = breaks ? (31 - __clz(breaks)) : -1;

  uint32_t running = valid ? value : kNoSlot;
  for (int d = 1; d < 32; d <<= 1) {
    const uint32_t other = __shfl_up_sync(0xFFFFFFFFu, running, d);
    if (lane >= d && (lane - d) > last_break) running = min(running, other);
  }
  // A segment reaching lane 0 continues the run the previous window ended on.
  if (valid && last_break < 0) running = min(running, carry_in);
  return running;
}

__global__ void LabelRowSweepKernel(uint32_t *__restrict__ labels,
                                    int width, int lo_y, int hi_y,
                                    int *__restrict__ changed) {
  const int lane = threadIdx.x & 31;
  const int warp_in_block = threadIdx.x >> 5;
  const int y = lo_y + blockIdx.x * (blockDim.x >> 5) + warp_in_block;
  if (y > hi_y) return;

  const uint32_t row = static_cast<uint32_t>(y) * width;
  bool wrote = false;

  uint32_t carry = kNoSlot;
  for (int base = 0; base < width; base += 32) {
    const int x = base + lane;
    const bool inside = x < width;
    const uint32_t label = inside ? labels[row + x] : kNoSlot;
    const bool valid = inside && label != kNoSlot;
    const unsigned int valid_mask = __ballot_sync(0xFFFFFFFFu, valid);

    const uint32_t running = SegmentedMinScan(label, valid, valid_mask, lane, carry);
    if (valid && running < label) {
      labels[row + x] = running;
      wrote = true;
    }
    // What the next window inherits: the last lane's value, if its run reaches
    // the window edge unbroken.
    const uint32_t edge = __shfl_sync(0xFFFFFFFFu, running, 31);
    carry = (valid_mask & 0x80000000u) ? edge : kNoSlot;
  }

  // Right to left, so a minimum sitting at the right of a run reaches the rest
  // of it without waiting for another round.
  carry = kNoSlot;
  for (int base = ((width + 31) / 32) * 32 - 32; base >= 0; base -= 32) {
    const int x = base + (31 - lane);
    const bool inside = x < width;
    const uint32_t label = inside ? labels[row + x] : kNoSlot;
    const bool valid = inside && label != kNoSlot;
    const unsigned int valid_mask = __ballot_sync(0xFFFFFFFFu, valid);

    const uint32_t running = SegmentedMinScan(label, valid, valid_mask, lane, carry);
    if (valid && running < label) {
      labels[row + x] = running;
      wrote = true;
    }
    const uint32_t edge = __shfl_sync(0xFFFFFFFFu, running, 31);
    carry = (valid_mask & 0x80000000u) ? edge : kNoSlot;
  }

  if (__any_sync(0xFFFFFFFFu, wrote) && lane == 0) *changed = 1;
}

__global__ void LabelColSweepKernel(uint32_t *__restrict__ labels,
                                    int width, int lo_y, int hi_y,
                                    int chunk, int *__restrict__ changed) {
  const int rows = hi_y - lo_y + 1;
  const int chunks_per_col = (rows + chunk - 1) / chunk;
  const int tid = blockIdx.x * blockDim.x + threadIdx.x;
  const int x = tid / chunks_per_col;
  const int chunk_index = tid % chunks_per_col;
  if (x >= width) return;

  const int from = lo_y + chunk_index * chunk;
  const int to = min(from + chunk, hi_y + 1);
  if (from >= to) return;

  bool wrote = false;

  uint32_t running = (from > lo_y)
      ? labels[static_cast<uint32_t>(from - 1) * width + x] : kNoSlot;
  for (int y = from; y < to; y++) {
    const uint32_t idx = static_cast<uint32_t>(y) * width + x;
    const uint32_t label = labels[idx];
    if (label == kNoSlot) {
      running = kNoSlot;
      continue;
    }
    running = min(running, label);
    if (running < label) {
      labels[idx] = running;
      wrote = true;
    }
  }

  running = (to <= hi_y) ? labels[static_cast<uint32_t>(to) * width + x] : kNoSlot;
  for (int y = to - 1; y >= from; y--) {
    const uint32_t idx = static_cast<uint32_t>(y) * width + x;
    const uint32_t label = labels[idx];
    if (label == kNoSlot) {
      running = kNoSlot;
      continue;
    }
    running = min(running, label);
    if (running < label) {
      labels[idx] = running;
      wrote = true;
    }
  }

  if (wrote) *changed = 1;
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

  uint32_t slot = kMaxBlobs;
  if (x < width && y <= hi_y) {
    const uint32_t idx = static_cast<uint32_t>(y) * width + x;
    const uint32_t label = labels[idx];
    if (label != kNoSlot) slot = slot_map[label];
  }
  const bool work = slot < kMaxBlobs;

  const unsigned int active = __ballot_sync(0xFFFFFFFFu, work);
  if (!active) return;

  // Seven atomics per alarmed pixel onto a handful of slots is most of this
  // kernel's time. A warp walks pixels along a row, so its lanes nearly always
  // sit in the same blob: when they do, reduce within the warp and let one lane
  // post the result. Mixed warps, at blob edges, fall back to per-lane atomics.
  const int leader = __ffs(active) - 1;
  const uint32_t leader_slot = __shfl_sync(0xFFFFFFFFu, slot, leader);
  const bool uniform =
      __ballot_sync(0xFFFFFFFFu, work && slot == leader_slot) == active;

  if (uniform) {
    unsigned int count = work ? 1u : 0u;
    int min_x = work ? x : INT_MAX;
    int max_x = work ? x : INT_MIN;
    int min_y = work ? y : INT_MAX;
    int max_y = work ? y : INT_MIN;
    unsigned long long x_total = work ? static_cast<unsigned long long>(x) : 0ull;
    unsigned long long y_total = work ? static_cast<unsigned long long>(y) : 0ull;

    for (int offset = 16; offset > 0; offset >>= 1) {
      count += __shfl_down_sync(0xFFFFFFFFu, count, offset);
      min_x = min(min_x, __shfl_down_sync(0xFFFFFFFFu, min_x, offset));
      max_x = max(max_x, __shfl_down_sync(0xFFFFFFFFu, max_x, offset));
      min_y = min(min_y, __shfl_down_sync(0xFFFFFFFFu, min_y, offset));
      max_y = max(max_y, __shfl_down_sync(0xFFFFFFFFu, max_y, offset));
      x_total += __shfl_down_sync(0xFFFFFFFFu, x_total, offset);
      y_total += __shfl_down_sync(0xFFFFFFFFu, y_total, offset);
    }

    if ((threadIdx.x & 31u) == 0) {
      atomicAdd(&counts[leader_slot], count);
      atomicMin(&lo_xs[leader_slot], min_x);
      atomicMax(&hi_xs[leader_slot], max_x);
      atomicMin(&lo_ys[leader_slot], min_y);
      atomicMax(&hi_ys[leader_slot], max_y);
      atomicAdd(&x_sums[leader_slot], x_total);
      atomicAdd(&y_sums[leader_slot], y_total);
    }
    return;
  }

  if (!work) return;
  atomicAdd(&counts[slot], 1u);
  atomicMin(&lo_xs[slot], x);
  atomicMax(&hi_xs[slot], x);
  atomicMin(&lo_ys[slot], y);
  atomicMax(&hi_ys[slot], y);
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
    if (stream) {
      cudaStreamDestroy(stream);
      stream = nullptr;
    }
    ref = delta = nullptr;
    labels = slot_map = nullptr;
    scratch = nullptr;
  }

  CUcontext context = nullptr;
  // One stream for the whole pipeline: the kernels are a chain of dependencies
  // anyway, and keeping them off the default stream means the readbacks can be
  // async and a frame costs a handful of syncs rather than one per copy.
  cudaStream_t stream = nullptr;
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

  cudaError_t err = cudaStreamCreate(&impl_->stream);
  if (err != cudaSuccess) {
    impl_->last_error = std::string("Failed to create a CUDA stream: ") + cudaGetErrorString(err);
    return false;
  }

  err = cudaMallocPitch(reinterpret_cast<void **>(&impl_->ref),
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
  cudaStream_t stream = impl_->stream;
  DeltaKernel<<<Grid2D(width_, height_, block), block, 0, stream>>>(
      y_plane, pitch, impl_->ref, impl_->ref_pitch,
      impl_->delta, impl_->delta_pitch, width_, height_);

  results.assign(impl_->zones.size(), ZoneResult());

  // Inactive zones first: they blank their area of the delta so no zone checked
  // below sees motion there, which is what Image::Fill does on the CPU path.
  for (const Impl::Zone &zone : impl_->zones) {
    if (!zone.inactive || zone.hi_y < zone.lo_y) continue;
    const dim3 zone_grid = Grid2D(width_, zone.hi_y - zone.lo_y + 1, block);
    MaskOutKernel<<<zone_grid, block, 0, stream>>>(impl_->delta, impl_->delta_pitch,
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

    cudaMemsetAsync(counters, 0, sizeof(unsigned long long) * 8, stream);
    ThresholdKernel<<<zone_grid, block, 0, stream>>>(
        impl_->delta, impl_->delta_pitch, zone.mask, zone.mask_pitch,
        zone.alarm, zone.alarm_pitch, width_, zone.lo_y, zone.hi_y,
        zone.min_threshold, zone.max_threshold, alarm_count, diff_sum);

    const uint8_t *stage = zone.alarm;
    size_t stage_pitch = zone.alarm_pitch;

    if (zone.want_filter) {
      FilterKernel<<<zone_grid, block, 0, stream>>>(
          zone.alarm, zone.alarm_pitch, zone.filtered, zone.filtered_pitch,
          zone.row_lo_x, zone.row_hi_x, width_, zone.lo_y, zone.hi_y,
          zone.filter_box_x, zone.filter_box_y, filter_count);
      stage = zone.filtered;
      stage_pitch = zone.filtered_pitch;
    }

    uint32_t host_blob_count = 0;
    if (zone.want_blobs) {
      LabelInitKernel<<<zone_grid, block, 0, stream>>>(stage, stage_pitch, impl_->labels,
                                            width_, zone.lo_y, zone.hi_y);
      // One thread per row, then one per column, alternating until nothing
      // moves. Each sweep is a launch and one 4-byte readback; a sweep settles
      // a whole run at once, so this is a few rounds rather than one per pixel
      // of the widest blob.
      // Chunk length trades rounds against parallelism: shorter chunks mean
      // more threads but more rounds for a blob that spans several. 128 keeps
      // a 1080p frame in the tens of thousands of threads while most blobs
      // still fit inside one chunk.
      constexpr int kSweepChunk = 128;
      const dim3 line_block(128);
      const int col_chunks = (rows + kSweepChunk - 1) / kSweepChunk;
      // One warp per row, so a block of 128 threads covers four rows.
      const int rows_per_block = line_block.x / 32;
      const dim3 row_grid((rows + rows_per_block - 1) / rows_per_block);
      const dim3 col_grid((width_ * col_chunks + line_block.x - 1) / line_block.x);
      int iterations = 0;
      for (; iterations < kMaxLabelIterations; iterations++) {
        cudaMemsetAsync(changed, 0, sizeof(int), stream);
        LabelRowSweepKernel<<<row_grid, line_block, 0, stream>>>(
            impl_->labels, width_, zone.lo_y, zone.hi_y, changed);
        LabelColSweepKernel<<<col_grid, line_block, 0, stream>>>(
            impl_->labels, width_, zone.lo_y, zone.hi_y, kSweepChunk, changed);
        int host_changed = 0;
        cudaMemcpyAsync(&host_changed, changed, sizeof(int), cudaMemcpyDeviceToHost, stream);
        cudaStreamSynchronize(stream);
        if (!host_changed) break;
      }
      if (iterations >= kMaxLabelIterations) result.blobs_truncated = true;

      // slot_map is indexed by label, and labels only exist in this zone's
      // rows, so clearing the whole plane was 33MB of writes per zone per frame
      // at 4K for a zone that may cover a tenth of it.
      cudaMemsetAsync(impl_->slot_map + static_cast<size_t>(zone.lo_y) * width_, 0xFF,
                      static_cast<size_t>(rows) * width_ * sizeof(uint32_t), stream);
      cudaMemsetAsync(blob_counts, 0, kMaxBlobs * sizeof(uint32_t), stream);
      // Bounding boxes start inverted so atomicMin/atomicMax build them up.
      cudaMemsetAsync(blob_lo_x, 0x7F, kMaxBlobs * sizeof(int), stream);
      cudaMemsetAsync(blob_lo_y, 0x7F, kMaxBlobs * sizeof(int), stream);
      cudaMemsetAsync(blob_hi_x, 0x80, kMaxBlobs * sizeof(int), stream);
      cudaMemsetAsync(blob_hi_y, 0x80, kMaxBlobs * sizeof(int), stream);
      cudaMemsetAsync(blob_x_sum, 0, kMaxBlobs * sizeof(unsigned long long), stream);
      cudaMemsetAsync(blob_y_sum, 0, kMaxBlobs * sizeof(unsigned long long), stream);

      BlobSlotKernel<<<zone_grid, block, 0, stream>>>(impl_->labels, impl_->slot_map,
                                           width_, zone.lo_y, zone.hi_y, blob_count);
      BlobStatsKernel<<<zone_grid, block, 0, stream>>>(impl_->labels, impl_->slot_map,
                                            width_, zone.lo_y, zone.hi_y,
                                            blob_counts, blob_lo_x, blob_hi_x,
                                            blob_lo_y, blob_hi_y,
                                            blob_x_sum, blob_y_sum);
      cudaMemcpyAsync(&host_blob_count, blob_count, sizeof(uint32_t), cudaMemcpyDeviceToHost, stream);
      cudaStreamSynchronize(stream);
    }

    // alarm count, diff sum, filter count and blob count are consecutive in the
    // scratch allocation, so they come back in one transfer rather than three.
    unsigned long long host_counters[4] = {0, 0, 0, 0};
    cudaMemcpyAsync(host_counters, counters, sizeof(host_counters), cudaMemcpyDeviceToHost, stream);
    cudaStreamSynchronize(stream);

    result.alarm_pixels = static_cast<uint32_t>(host_counters[0]);
    result.pixel_diff_sum = host_counters[1];
    result.filter_pixels = zone.want_filter ? static_cast<uint32_t>(host_counters[2])
                                            : result.alarm_pixels;

    if (zone.want_blobs && host_blob_count) {
      if (host_blob_count > kMaxBlobs) {
        result.blobs_truncated = true;
        host_blob_count = kMaxBlobs;
      }
      std::vector<uint32_t> counts(host_blob_count);
      std::vector<int> lo_x(host_blob_count), hi_x(host_blob_count);
      std::vector<int> lo_y(host_blob_count), hi_y(host_blob_count);
      std::vector<unsigned long long> x_sum(host_blob_count), y_sum(host_blob_count);
      cudaMemcpyAsync(counts.data(), blob_counts, host_blob_count * sizeof(uint32_t), cudaMemcpyDeviceToHost, stream);
      cudaMemcpyAsync(lo_x.data(), blob_lo_x, host_blob_count * sizeof(int), cudaMemcpyDeviceToHost, stream);
      cudaMemcpyAsync(hi_x.data(), blob_hi_x, host_blob_count * sizeof(int), cudaMemcpyDeviceToHost, stream);
      cudaMemcpyAsync(lo_y.data(), blob_lo_y, host_blob_count * sizeof(int), cudaMemcpyDeviceToHost, stream);
      cudaMemcpyAsync(hi_y.data(), blob_hi_y, host_blob_count * sizeof(int), cudaMemcpyDeviceToHost, stream);
      cudaMemcpyAsync(x_sum.data(), blob_x_sum, host_blob_count * sizeof(unsigned long long), cudaMemcpyDeviceToHost, stream);
      cudaMemcpyAsync(y_sum.data(), blob_y_sum, host_blob_count * sizeof(unsigned long long), cudaMemcpyDeviceToHost, stream);
      cudaStreamSynchronize(stream);

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

  cudaError_t err = cudaStreamSynchronize(stream);
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
