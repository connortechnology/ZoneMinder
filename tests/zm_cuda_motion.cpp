/*
 * This file is part of the ZoneMinder Project. See AUTHORS file for maintainers.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "zm_catch2.h"

#include "zm_cuda_motion.h"

#if HAVE_CUDA

#include <cuda_runtime.h>

#include <chrono>
#include <cstring>
#include <random>
#include <vector>

// The point of these is that the kernels agree with the CPU motion detection
// they stand in for. Each expectation below is computed on the host with the
// same arithmetic zm_zone.cpp and zm_image.cpp use, so a kernel that drifts
// fails here rather than quietly scoring monitors differently.

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;

struct DevicePlane {
  uint8_t *data = nullptr;
  size_t pitch = 0;

  DevicePlane(int width, int height) {
    cudaMallocPitch(reinterpret_cast<void **>(&data), &pitch, width, height);
  }
  ~DevicePlane() { cudaFree(data); }

  void Upload(const std::vector<uint8_t> &host, int width, int height) {
    cudaMemcpy2D(data, pitch, host.data(), width, width, height, cudaMemcpyHostToDevice);
  }
};

// A full-width rectangular zone, with the per-row spans Zone keeps in ranges[].
struct HostZone {
  std::vector<uint8_t> mask;
  std::vector<int> row_lo_x;
  std::vector<int> row_hi_x;
  int lo_y = 0;
  int hi_y = 0;

  HostZone(int lo_x_in, int lo_y_in, int hi_x_in, int hi_y_in)
      : mask(kWidth * kHeight, 0), row_lo_x(kHeight, -1), row_hi_x(kHeight, -1),
        lo_y(lo_y_in), hi_y(hi_y_in) {
    for (int y = lo_y_in; y <= hi_y_in; y++) {
      row_lo_x[y] = lo_x_in;
      row_hi_x[y] = hi_x_in;
      for (int x = lo_x_in; x <= hi_x_in; x++) mask[y * kWidth + x] = 0xFF;
    }
  }

  zm::cuda::ZoneSpec Spec(uint8_t min_threshold, uint8_t max_threshold) const {
    zm::cuda::ZoneSpec spec;
    spec.mask = mask.data();
    spec.row_lo_x = row_lo_x.data();
    spec.row_hi_x = row_hi_x.data();
    spec.lo_y = lo_y;
    spec.hi_y = hi_y;
    spec.min_pixel_threshold = min_threshold;
    spec.max_pixel_threshold = max_threshold;
    return spec;
  }
};

std::vector<uint8_t> RandomPlane(unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> dist(0, 255);
  std::vector<uint8_t> plane(kWidth * kHeight);
  for (uint8_t &pixel : plane) pixel = static_cast<uint8_t>(dist(rng));
  return plane;
}

// alarmedpixels_row, on the host, for the expectation.
void HostThreshold(const std::vector<uint8_t> &cur, const std::vector<uint8_t> &ref,
                   const HostZone &zone, uint8_t min_threshold, uint8_t max_threshold,
                   uint32_t &count, uint64_t &sum, std::vector<uint8_t> *mask_out = nullptr) {
  const uint8_t calc_max = max_threshold ? max_threshold : 255;
  count = 0;
  sum = 0;
  if (mask_out) mask_out->assign(kWidth * kHeight, 0);

  for (int y = zone.lo_y; y <= zone.hi_y; y++) {
    for (int x = 0; x < kWidth; x++) {
      const int i = y * kWidth + x;
      const uint8_t d = static_cast<uint8_t>(abs(cur[i] - ref[i]));
      const bool alarmed = (zone.mask[i] != 0) && (d > min_threshold) && (d <= calc_max);
      if (alarmed) {
        count++;
        sum += d;
        if (mask_out) (*mask_out)[i] = 255;
      }
    }
  }
}

}  // namespace

TEST_CASE("CUDA motion: thresholded pixel counts match the CPU rule", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  const std::vector<uint8_t> reference = RandomPlane(1);
  const std::vector<uint8_t> current = RandomPlane(2);
  const HostZone zone(10, 20, kWidth - 30, kHeight - 40);

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));
  REQUIRE(detector.SetZones({zone.Spec(25, 200)}));

  DevicePlane device_reference(kWidth, kHeight);
  DevicePlane device_current(kWidth, kHeight);
  device_reference.Upload(reference, kWidth, kHeight);
  device_current.Upload(current, kWidth, kHeight);

  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));
  REQUIRE(results.size() == 1);

  uint32_t expected_count = 0;
  uint64_t expected_sum = 0;
  HostThreshold(current, reference, zone, 25, 200, expected_count, expected_sum);

  REQUIRE(expected_count > 0);
  REQUIRE(results[0].alarm_pixels == expected_count);
  REQUIRE(results[0].pixel_diff_sum == expected_sum);
}

TEST_CASE("CUDA motion: the downloaded mask matches the CPU mask", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  const std::vector<uint8_t> reference = RandomPlane(3);
  const std::vector<uint8_t> current = RandomPlane(4);
  const HostZone zone(0, 0, kWidth - 1, kHeight - 1);

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));
  REQUIRE(detector.SetZones({zone.Spec(40, 0)}));

  DevicePlane device_reference(kWidth, kHeight);
  DevicePlane device_current(kWidth, kHeight);
  device_reference.Upload(reference, kWidth, kHeight);
  device_current.Upload(current, kWidth, kHeight);
  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));

  uint32_t expected_count = 0;
  uint64_t expected_sum = 0;
  std::vector<uint8_t> expected_mask;
  HostThreshold(current, reference, zone, 40, 0, expected_count, expected_sum, &expected_mask);

  std::vector<uint8_t> mask(kWidth * kHeight, 0);
  REQUIRE(detector.DownloadMask(0, mask.data(), kWidth));
  REQUIRE(mask == expected_mask);
}

TEST_CASE("CUDA motion: a pixel below the minimum never alarms", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  // Every pixel differs by exactly the minimum threshold, which the CPU rule
  // treats as quiet: the test is d > min, not d >= min.
  std::vector<uint8_t> reference(kWidth * kHeight, 100);
  std::vector<uint8_t> current(kWidth * kHeight, 130);
  const HostZone zone(0, 0, kWidth - 1, kHeight - 1);

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));
  REQUIRE(detector.SetZones({zone.Spec(30, 0)}));

  DevicePlane device_reference(kWidth, kHeight);
  DevicePlane device_current(kWidth, kHeight);
  device_reference.Upload(reference, kWidth, kHeight);
  device_current.Upload(current, kWidth, kHeight);
  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));
  REQUIRE(results[0].alarm_pixels == 0);

  // One more, and the whole zone alarms.
  current.assign(kWidth * kHeight, 131);
  device_current.Upload(current, kWidth, kHeight);
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));
  REQUIRE(results[0].alarm_pixels == static_cast<uint32_t>(kWidth * kHeight));
}

TEST_CASE("CUDA motion: components are counted and measured", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  // Three separated rectangles on a quiet background: 4-connected labelling
  // should find exactly three, with their own sizes and bounding boxes.
  std::vector<uint8_t> reference(kWidth * kHeight, 10);
  std::vector<uint8_t> current(kWidth * kHeight, 10);

  struct Rect { int lo_x, lo_y, hi_x, hi_y; };
  const std::vector<Rect> rects = {{10, 10, 19, 19}, {50, 30, 79, 49}, {200, 100, 204, 104}};
  for (const Rect &rect : rects)
    for (int y = rect.lo_y; y <= rect.hi_y; y++)
      for (int x = rect.lo_x; x <= rect.hi_x; x++) current[y * kWidth + x] = 200;

  const HostZone zone(0, 0, kWidth - 1, kHeight - 1);
  zm::cuda::ZoneSpec spec = zone.Spec(20, 0);
  spec.want_blobs = true;

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));
  REQUIRE(detector.SetZones({spec}));

  DevicePlane device_reference(kWidth, kHeight);
  DevicePlane device_current(kWidth, kHeight);
  device_reference.Upload(reference, kWidth, kHeight);
  device_current.Upload(current, kWidth, kHeight);
  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));
  REQUIRE_FALSE(results[0].blobs_truncated);
  REQUIRE(results[0].blobs.size() == rects.size());

  for (const Rect &rect : rects) {
    const uint32_t area = (rect.hi_x - rect.lo_x + 1) * (rect.hi_y - rect.lo_y + 1);
    bool found = false;
    for (const zm::cuda::Blob &blob : results[0].blobs) {
      if (blob.lo_x != rect.lo_x || blob.lo_y != rect.lo_y) continue;
      found = true;
      CHECK(blob.count == area);
      CHECK(blob.hi_x == rect.hi_x);
      CHECK(blob.hi_y == rect.hi_y);
    }
    CHECK(found);
  }
}

TEST_CASE("CUDA motion: an L shape stays one component", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  // Diagonal-only contact must NOT join, matching the CPU pass, which only
  // looks left and up. The L's two arms touch edge to edge, so they are one
  // component; the lone diagonal neighbour beyond its corner is another.
  std::vector<uint8_t> reference(kWidth * kHeight, 0);
  std::vector<uint8_t> current(kWidth * kHeight, 0);

  for (int y = 20; y <= 60; y++) current[y * kWidth + 30] = 255;   // vertical arm
  for (int x = 30; x <= 70; x++) current[60 * kWidth + x] = 255;   // horizontal arm
  current[61 * kWidth + 71] = 255;                                 // diagonal from the tip

  const HostZone zone(0, 0, kWidth - 1, kHeight - 1);
  zm::cuda::ZoneSpec spec = zone.Spec(20, 0);
  spec.want_blobs = true;

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));
  REQUIRE(detector.SetZones({spec}));

  DevicePlane device_reference(kWidth, kHeight);
  DevicePlane device_current(kWidth, kHeight);
  device_reference.Upload(reference, kWidth, kHeight);
  device_current.Upload(current, kWidth, kHeight);
  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));
  REQUIRE(results[0].blobs.size() == 2);

  uint32_t largest = 0;
  for (const zm::cuda::Blob &blob : results[0].blobs) largest = std::max(largest, blob.count);
  // 41 vertical + 41 horizontal, sharing the corner pixel.
  REQUIRE(largest == 81);
}

TEST_CASE("CUDA motion: the box filter drops isolated pixels", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  std::vector<uint8_t> reference(kWidth * kHeight, 0);
  std::vector<uint8_t> current(kWidth * kHeight, 0);

  // A solid 10x10 square survives a 3x3 filter; single pixels scattered around
  // it do not.
  for (int y = 100; y < 110; y++)
    for (int x = 100; x < 110; x++) current[y * kWidth + x] = 255;
  current[10 * kWidth + 10] = 255;
  current[20 * kWidth + 40] = 255;
  current[200 * kWidth + 300] = 255;

  const HostZone zone(0, 0, kWidth - 1, kHeight - 1);
  zm::cuda::ZoneSpec spec = zone.Spec(20, 0);
  spec.want_filter = true;
  spec.filter_box_x = 3;
  spec.filter_box_y = 3;

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));
  REQUIRE(detector.SetZones({spec}));

  DevicePlane device_reference(kWidth, kHeight);
  DevicePlane device_current(kWidth, kHeight);
  device_reference.Upload(reference, kWidth, kHeight);
  device_current.Upload(current, kWidth, kHeight);
  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));

  REQUIRE(results[0].alarm_pixels == 103);   // the square plus the three strays
  REQUIRE(results[0].filter_pixels == 100);  // strays gone, square intact
}

TEST_CASE("CUDA motion: the fast blend matches std_fastblend", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  const std::vector<uint8_t> reference = RandomPlane(5);
  const std::vector<uint8_t> current = RandomPlane(6);

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));

  DevicePlane device_reference(kWidth, kHeight);
  DevicePlane device_current(kWidth, kHeight);
  device_reference.Upload(reference, kWidth, kHeight);
  device_current.Upload(current, kWidth, kHeight);
  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  // 6.25% lands on a shift of 4, the same table std_fastblend uses.
  REQUIRE(detector.BlendReference(device_current.data, device_current.pitch, 6.25, true));

  std::vector<uint8_t> blended(kWidth * kHeight, 0);
  REQUIRE(detector.DownloadReference(blended.data(), kWidth));

  for (size_t i = 0; i < blended.size(); i++) {
    const int r = reference[i];
    const int c = current[i];
    const uint8_t expected = static_cast<uint8_t>(r + ((c - r) >> 4));
    if (blended[i] != expected) {
      FAIL("pixel " << i << ": expected " << static_cast<int>(expected)
                    << " got " << static_cast<int>(blended[i]));
    }
  }
  SUCCEED();
}

TEST_CASE("CUDA motion: a serpentine stays one component", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  // The shape the sweeps have to work hardest on: a snake whose ends are far
  // apart along the path but close in the plane. Each sweep carries a label
  // along one leg, so this needs several rounds where a solid blob needs one.
  std::vector<uint8_t> reference(kWidth * kHeight, 0);
  std::vector<uint8_t> current(kWidth * kHeight, 0);

  int expected = 0;
  const int rows = 8;
  for (int leg = 0; leg < rows; leg++) {
    const int y = 20 + leg * 20;
    const int from = (leg % 2 == 0) ? 20 : kWidth - 40;
    const int to = (leg % 2 == 0) ? kWidth - 40 : 20;
    const int step = (from < to) ? 1 : -1;
    for (int x = from; x != to + step; x += step) {
      current[y * kWidth + x] = 255;
      expected++;
    }
    // The riser joining this leg to the next.
    if (leg + 1 < rows) {
      for (int yy = y + 1; yy < y + 20; yy++) {
        current[yy * kWidth + to] = 255;
        expected++;
      }
    }
  }

  const HostZone zone(0, 0, kWidth - 1, kHeight - 1);
  zm::cuda::ZoneSpec spec = zone.Spec(20, 0);
  spec.want_blobs = true;

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));
  REQUIRE(detector.SetZones({spec}));

  DevicePlane device_reference(kWidth, kHeight);
  DevicePlane device_current(kWidth, kHeight);
  device_reference.Upload(reference, kWidth, kHeight);
  device_current.Upload(current, kWidth, kHeight);
  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));
  REQUIRE_FALSE(results[0].blobs_truncated);
  REQUIRE(results[0].blobs.size() == 1);
  REQUIRE(results[0].blobs[0].count == static_cast<uint32_t>(expected));
}

TEST_CASE("CUDA motion: an unaligned zone counts exactly", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  // Nothing here is a multiple of 32: the zone starts at x=7 and is 38 wide,
  // so warps are partial. The kernels reduce across the whole warp, and a lane
  // that has left the kernel cannot take part in that, so out of range lanes
  // have to stay and contribute nothing instead of returning early.
  std::vector<uint8_t> reference(kWidth * kHeight, 30);
  std::vector<uint8_t> current(kWidth * kHeight, 30);

  // A 13x11 patch inside the zone, and one outside it that must not count.
  uint32_t expected = 0;
  for (int y = 9; y < 20; y++)
    for (int x = 11; x < 24; x++) { current[y * kWidth + x] = 210; expected++; }
  for (int y = 9; y < 20; y++)
    for (int x = 100; x < 120; x++) current[y * kWidth + x] = 210;

  const HostZone zone(7, 5, 44, 43);
  zm::cuda::ZoneSpec spec = zone.Spec(20, 0);
  spec.want_blobs = true;

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));
  REQUIRE(detector.SetZones({spec}));

  DevicePlane device_reference(kWidth, kHeight);
  DevicePlane device_current(kWidth, kHeight);
  device_reference.Upload(reference, kWidth, kHeight);
  device_current.Upload(current, kWidth, kHeight);
  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));

  REQUIRE(results[0].alarm_pixels == expected);
  REQUIRE(results[0].blobs.size() == 1);
  REQUIRE(results[0].blobs[0].count == expected);
  REQUIRE(results[0].blobs[0].lo_x == 11);
  REQUIRE(results[0].blobs[0].hi_x == 23);
  REQUIRE(results[0].blobs[0].lo_y == 9);
  REQUIRE(results[0].blobs[0].hi_y == 19);
}

// Hidden by default (the leading dot); run with ./tests "[.cudabench]". This is
// the workload the component pass is sensitive to: wide solid blobs, where a
// labelling scheme that moves one pixel per round pays for every pixel of the
// widest one.
TEST_CASE("CUDA motion: component pass throughput", "[.cudabench]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  constexpr int kBenchWidth = 1920;
  constexpr int kBenchHeight = 1080;
  constexpr int kFrames = 50;

  std::vector<uint8_t> reference(kBenchWidth * kBenchHeight, 20);
  std::vector<uint8_t> current = reference;
  // Forty 90x60 rectangles: wide enough that per-pixel propagation needs ~90
  // rounds to settle each one.
  for (int row = 0; row < 5; row++) {
    for (int col = 0; col < 8; col++) {
      const int x0 = 60 + col * 220;
      const int y0 = 60 + row * 200;
      for (int y = y0; y < y0 + 60; y++)
        for (int x = x0; x < x0 + 90; x++) current[y * kBenchWidth + x] = 200;
    }
  }

  std::vector<uint8_t> mask(kBenchWidth * kBenchHeight, 0xFF);
  std::vector<int> row_lo(kBenchHeight, 0), row_hi(kBenchHeight, kBenchWidth - 1);

  zm::cuda::ZoneSpec spec;
  spec.mask = mask.data();
  spec.row_lo_x = row_lo.data();
  spec.row_hi_x = row_hi.data();
  spec.lo_y = 0;
  spec.hi_y = kBenchHeight - 1;
  spec.min_pixel_threshold = 20;
  spec.want_filter = true;
  spec.filter_box_x = 3;
  spec.filter_box_y = 3;
  spec.want_blobs = true;

  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kBenchWidth, kBenchHeight));
  REQUIRE(detector.SetZones({spec}));

  uint8_t *device_reference = nullptr, *device_current = nullptr;
  size_t reference_pitch = 0, current_pitch = 0;
  cudaMallocPitch(reinterpret_cast<void **>(&device_reference), &reference_pitch, kBenchWidth, kBenchHeight);
  cudaMallocPitch(reinterpret_cast<void **>(&device_current), &current_pitch, kBenchWidth, kBenchHeight);
  cudaMemcpy2D(device_reference, reference_pitch, reference.data(), kBenchWidth,
               kBenchWidth, kBenchHeight, cudaMemcpyHostToDevice);
  cudaMemcpy2D(device_current, current_pitch, current.data(), kBenchWidth,
               kBenchWidth, kBenchHeight, cudaMemcpyHostToDevice);
  REQUIRE(detector.AssignReference(device_reference, reference_pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current, current_pitch, results));
  REQUIRE(results[0].blobs.size() == 40);

  // Timed in stages so the cost lands on a stage rather than on a guess:
  // threshold only, then with the filter, then with components as well.
  auto time_config = [&](const char *what, bool filter, bool blobs) {
    zm::cuda::ZoneSpec staged = spec;
    staged.want_filter = filter;
    staged.want_blobs = blobs;
    REQUIRE(detector.SetZones({staged}));
    REQUIRE(detector.AssignReference(device_reference, reference_pitch));
    REQUIRE(detector.Detect(device_current, current_pitch, results));

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; i++) {
      REQUIRE(detector.Detect(device_current, current_pitch, results));
    }
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count() / kFrames;
    WARN(what << ": " << ms << " ms/frame");
    return ms;
  };

  const double threshold_ms = time_config("delta + threshold", false, false);
  const double filter_ms = time_config("+ 3x3 filter", true, false);
  const double blobs_ms = time_config("+ components", true, true);

  WARN("stage costs: threshold " << threshold_ms
       << ", filter " << (filter_ms - threshold_ms)
       << ", components " << (blobs_ms - filter_ms) << " ms");

  // Blobs spread over the whole frame is the worst case for a labelling stage
  // that bounds itself to where the motion is. What a camera usually sees is a
  // person or a car in part of the view, and an empty frame most of the time.
  auto retime = [&](const char *what, const std::vector<uint8_t> &frame) {
    cudaMemcpy2D(device_current, current_pitch, frame.data(), kBenchWidth,
                 kBenchWidth, kBenchHeight, cudaMemcpyHostToDevice);
    REQUIRE(detector.AssignReference(device_reference, reference_pitch));
    REQUIRE(detector.Detect(device_current, current_pitch, results));

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kFrames; i++) {
      REQUIRE(detector.Detect(device_current, current_pitch, results));
    }
    const auto end = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(end - start).count() / kFrames;
    WARN(what << ": " << ms << " ms/frame (" << results[0].blobs.size() << " blobs)");
  };

  std::vector<uint8_t> localised(kBenchWidth * kBenchHeight, 20);
  for (int y = 700; y < 900; y++)
    for (int x = 1400; x < 1700; x++) localised[y * kBenchWidth + x] = 200;

  const std::vector<uint8_t> quiet(kBenchWidth * kBenchHeight, 20);

  retime("motion across the frame", current);
  retime("motion in one corner", localised);
  retime("quiet frame", quiet);

  cudaFree(device_reference);
  cudaFree(device_current);
}

#endif  // HAVE_CUDA
