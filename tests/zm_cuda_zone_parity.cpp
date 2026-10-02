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

#include "zm_monitor.h"
#include "zm_zone.h"

#if HAVE_CUDA

#include <cuda_runtime.h>

#include <memory>
#include <random>
#include <vector>

// The question these answer is the one that matters for wiring the kernels in:
// does a zone score the same whether its pixels were counted on the CPU or on
// the card? Each case runs identical frames through Zone::CheckAlarms and
// Zone::CheckAlarmsCuda and compares what the monitor would act on -- the
// score, the pixel counts, the blob counts and the alarm centre.

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;

void EnsureConfig() {
  if (!config.font_file_location) config.font_file_location = "";
  if (!config.event_close_mode) config.event_close_mode = "idle";
}

class TestMonitor : public Monitor {
 public:
  TestMonitor(unsigned int w, unsigned int h) : Monitor() {
    width = w;
    height = h;
    camera_width = w;
    camera_height = h;
    colours = ZM_COLOUR_GRAY8;
    savejpegs = 0;  // no highlight image, so neither path needs the mask back
  }
};

std::shared_ptr<Monitor> MakeMonitor() {
  return std::static_pointer_cast<Monitor>(std::make_shared<TestMonitor>(kWidth, kHeight));
}

Polygon FullFramePolygon() {
  std::vector<Vector2> vertices = {
      Vector2(0, 0), Vector2(kWidth - 1, 0),
      Vector2(kWidth - 1, kHeight - 1), Vector2(0, kHeight - 1),
  };
  return Polygon(vertices);
}

struct DevicePlane {
  uint8_t *data = nullptr;
  size_t pitch = 0;

  DevicePlane() { cudaMallocPitch(reinterpret_cast<void **>(&data), &pitch, kWidth, kHeight); }
  ~DevicePlane() { cudaFree(data); }

  void Upload(const std::vector<uint8_t> &host) {
    cudaMemcpy2D(data, pitch, host.data(), kWidth, kWidth, kHeight, cudaMemcpyHostToDevice);
  }
};

std::unique_ptr<Image> MakeImage(const std::vector<uint8_t> &plane) {
  auto image = std::unique_ptr<Image>(new Image(kWidth, kHeight, ZM_COLOUR_GRAY8, ZM_SUBPIX_ORDER_NONE));
  for (int y = 0; y < kHeight; y++)
    for (int x = 0; x < kWidth; x++) *image->Buffer(x, y) = plane[y * kWidth + x];
  return image;
}

// Runs one frame pair through both paths and reports the two sets of stats.
struct Parity {
  ZoneStats cpu{0};
  ZoneStats gpu{0};
  bool cpu_alarmed = false;
  bool gpu_alarmed = false;
};

Parity RunBoth(const std::vector<uint8_t> &reference,
               const std::vector<uint8_t> &current,
               Zone::CheckMethod method,
               int min_pixel_threshold,
               int min_alarm_pixels,
               int min_blob_pixels = 0) {
  EnsureConfig();
  auto monitor = MakeMonitor();

  Zone cpu_zone(monitor, 1, "cpu", Zone::ACTIVE, FullFramePolygon(), kRGBRed, method,
                min_pixel_threshold, 0, min_alarm_pixels, 0, Vector2(3, 3),
                0, 0, min_blob_pixels, 0, 0, 0, 0, 0);
  Zone gpu_zone(monitor, 1, "gpu", Zone::ACTIVE, FullFramePolygon(), kRGBRed, method,
                min_pixel_threshold, 0, min_alarm_pixels, 0, Vector2(3, 3),
                0, 0, min_blob_pixels, 0, 0, 0, 0, 0);

  Parity parity;

  // CPU: the delta the monitor would have built, then the zone's own pass.
  std::unique_ptr<Image> reference_image = MakeImage(reference);
  std::unique_ptr<Image> current_image = MakeImage(current);
  Image delta_image;
  REQUIRE(reference_image->Delta(*current_image, &delta_image));
  parity.cpu_alarmed = cpu_zone.CheckAlarms(&delta_image);
  parity.cpu = cpu_zone.GetStats();

  // Device: the same two frames, never leaving the card.
  zm::cuda::MotionDetector detector;
  REQUIRE(detector.Init(kWidth, kHeight));
  REQUIRE(detector.SetZones({gpu_zone.CudaSpec()}));

  DevicePlane device_reference, device_current;
  device_reference.Upload(reference);
  device_current.Upload(current);
  REQUIRE(detector.AssignReference(device_reference.data, device_reference.pitch));

  std::vector<zm::cuda::ZoneResult> results;
  REQUIRE(detector.Detect(device_current.data, device_current.pitch, results));
  REQUIRE(results.size() == 1);
  parity.gpu_alarmed = gpu_zone.CheckAlarmsCuda(results[0], &detector, 0);
  parity.gpu = gpu_zone.GetStats();

  return parity;
}

std::vector<uint8_t> RandomPlane(unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> dist(0, 255);
  std::vector<uint8_t> plane(kWidth * kHeight);
  for (uint8_t &pixel : plane) pixel = static_cast<uint8_t>(dist(rng));
  return plane;
}

}  // namespace

TEST_CASE("CUDA parity: alarmed pixel scoring matches the CPU zone", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  const Parity parity = RunBoth(RandomPlane(11), RandomPlane(12),
                                Zone::ALARMED_PIXELS, 30, 10);

  REQUIRE(parity.cpu_alarmed == parity.gpu_alarmed);
  REQUIRE(parity.cpu.alarm_pixels_ > 0);
  CHECK(parity.gpu.alarm_pixels_ == parity.cpu.alarm_pixels_);
  CHECK(parity.gpu.pixel_diff_ == parity.cpu.pixel_diff_);
  CHECK(parity.gpu.score_ == parity.cpu.score_);
}

TEST_CASE("CUDA parity: a quiet frame alarms in neither path", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  const std::vector<uint8_t> flat(kWidth * kHeight, 128);
  const Parity parity = RunBoth(flat, flat, Zone::ALARMED_PIXELS, 15, 50);

  CHECK_FALSE(parity.cpu_alarmed);
  CHECK_FALSE(parity.gpu_alarmed);
  CHECK(parity.gpu.score_ == parity.cpu.score_);
  CHECK(parity.gpu.alarm_pixels_ == parity.cpu.alarm_pixels_);
}

TEST_CASE("CUDA parity: too few alarmed pixels fails both paths", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  std::vector<uint8_t> reference(kWidth * kHeight, 40);
  std::vector<uint8_t> current(kWidth * kHeight, 40);
  // Twenty changed pixels against a minimum of a hundred.
  for (int i = 0; i < 20; i++) current[50 * kWidth + 50 + i] = 240;

  const Parity parity = RunBoth(reference, current, Zone::ALARMED_PIXELS, 20, 100);

  CHECK_FALSE(parity.cpu_alarmed);
  CHECK_FALSE(parity.gpu_alarmed);
  CHECK(parity.gpu.alarm_pixels_ == 20);
  CHECK(parity.gpu.alarm_pixels_ == parity.cpu.alarm_pixels_);
}

TEST_CASE("CUDA parity: blob scoring and alarm centre match on solid shapes", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  // Solid rectangles, well clear of each other and of the frame edge: shapes
  // where the CPU filter's in-place scan and the kernel's out-of-place one
  // agree, so the comparison is of the scoring rather than of that difference.
  std::vector<uint8_t> reference(kWidth * kHeight, 30);
  std::vector<uint8_t> current(kWidth * kHeight, 30);
  for (int y = 40; y < 80; y++)
    for (int x = 40; x < 100; x++) current[y * kWidth + x] = 220;
  for (int y = 150; y < 170; y++)
    for (int x = 200; x < 230; x++) current[y * kWidth + x] = 220;

  const Parity parity = RunBoth(reference, current, Zone::BLOBS, 20, 10, 10);

  REQUIRE(parity.cpu_alarmed);
  REQUIRE(parity.gpu_alarmed);
  CHECK(parity.gpu.alarm_pixels_ == parity.cpu.alarm_pixels_);
  CHECK(parity.gpu.alarm_filter_pixels_ == parity.cpu.alarm_filter_pixels_);
  CHECK(parity.gpu.alarm_blobs_ == parity.cpu.alarm_blobs_);
  CHECK(parity.gpu.alarm_blob_pixels_ == parity.cpu.alarm_blob_pixels_);
  CHECK(parity.gpu.min_blob_size_ == parity.cpu.min_blob_size_);
  CHECK(parity.gpu.max_blob_size_ == parity.cpu.max_blob_size_);
  CHECK(parity.gpu.score_ == parity.cpu.score_);
  CHECK(parity.gpu.alarm_centre_ == parity.cpu.alarm_centre_);
  CHECK(parity.gpu.alarm_box_.Lo() == parity.cpu.alarm_box_.Lo());
  CHECK(parity.gpu.alarm_box_.Hi() == parity.cpu.alarm_box_.Hi());
}

TEST_CASE("CUDA parity: blobs under the minimum are dropped by both", "[cuda]") {
  if (!zm::cuda::MotionDetector::Available()) {
    WARN("No CUDA device present, skipping");
    return;
  }

  std::vector<uint8_t> reference(kWidth * kHeight, 30);
  std::vector<uint8_t> current(kWidth * kHeight, 30);
  // One blob well over the minimum, one well under it.
  for (int y = 40; y < 70; y++)
    for (int x = 40; x < 70; x++) current[y * kWidth + x] = 220;
  for (int y = 150; y < 153; y++)
    for (int x = 200; x < 203; x++) current[y * kWidth + x] = 220;

  const Parity parity = RunBoth(reference, current, Zone::BLOBS, 20, 10, 100);

  REQUIRE(parity.cpu_alarmed);
  REQUIRE(parity.gpu_alarmed);
  CHECK(parity.cpu.alarm_blobs_ == 1);
  CHECK(parity.gpu.alarm_blobs_ == parity.cpu.alarm_blobs_);
  CHECK(parity.gpu.alarm_blob_pixels_ == parity.cpu.alarm_blob_pixels_);
  CHECK(parity.gpu.score_ == parity.cpu.score_);
}

#endif  // HAVE_CUDA
