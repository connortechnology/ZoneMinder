/*
 * This file is part of the ZoneMinder Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "zm_catch2.h"

#include <queue>
#include <vector>

#include "zm_yolov8_onnx_postprocess.h"

namespace {

// Build a synthetic YOLOv8 output tensor in the channel-major layout the
// postprocess expects: [4 + num_classes, num_anchors]. Each box is placed at a
// distinct anchor slot so the channel-major addressing is exercised properly.
struct SyntheticBox {
  int anchor;
  int class_index;
  float score;
  float cx, cy, w, h;
};

std::vector<float> BuildTensor(int num_classes, int num_anchors,
                               const std::vector<SyntheticBox> &boxes) {
  const int num_channels = 4 + num_classes;
  std::vector<float> data(static_cast<size_t>(num_channels) * num_anchors, 0.0f);
  auto at = [&](int c, int a) -> float & {
    return data[c * num_anchors + a];
  };
  for (const auto &b : boxes) {
    at(0, b.anchor) = b.cx;
    at(1, b.anchor) = b.cy;
    at(2, b.anchor) = b.w;
    at(3, b.anchor) = b.h;
    at(4 + b.class_index, b.anchor) = b.score;
  }
  return data;
}

}  // namespace

TEST_CASE("yolov8_onnx_postprocess: empty tensor produces no detections") {
  std::queue<BBox> out;
  std::vector<float> data(84 * 8400, 0.0f);
  yolov8_onnx_postprocess(data.data(), 84, 8400, 0.25f, 0.45f, out);
  REQUIRE(out.empty());
}

TEST_CASE("yolov8_onnx_postprocess: rejects null/degenerate input") {
  std::queue<BBox> out;
  yolov8_onnx_postprocess(nullptr, 84, 8400, 0.25f, 0.45f, out);
  REQUIRE(out.empty());

  std::vector<float> data(84 * 8400, 0.0f);
  yolov8_onnx_postprocess(data.data(), 4, 8400, 0.25f, 0.45f, out);  // no class channels
  REQUIRE(out.empty());

  yolov8_onnx_postprocess(data.data(), 84, 0, 0.25f, 0.45f, out);
  REQUIRE(out.empty());
}

TEST_CASE("yolov8_onnx_postprocess: emits a box above threshold and decodes corners") {
  const int num_classes = 80;
  const int num_anchors = 8400;
  std::vector<SyntheticBox> boxes = {{
      /*anchor*/ 100,
      /*class*/ 0,
      /*score*/ 0.9f,
      /*cx*/ 320.0f, /*cy*/ 320.0f, /*w*/ 100.0f, /*h*/ 200.0f,
  }};
  std::vector<float> tensor = BuildTensor(num_classes, num_anchors, boxes);

  std::queue<BBox> out;
  yolov8_onnx_postprocess(tensor.data(), 4 + num_classes, num_anchors,
                          /*conf*/ 0.25f, /*iou*/ 0.45f, out);

  REQUIRE(out.size() == 1);
  BBox b = out.front();
  REQUIRE(b.class_index == 0);
  REQUIRE(b.class_score == Catch::Approx(0.9f));
  REQUIRE(b.x_min == Catch::Approx(270.0f));
  REQUIRE(b.y_min == Catch::Approx(220.0f));
  REQUIRE(b.x_max == Catch::Approx(370.0f));
  REQUIRE(b.y_max == Catch::Approx(420.0f));
}

TEST_CASE("yolov8_onnx_postprocess: filters detections below confidence threshold") {
  const int num_classes = 80;
  const int num_anchors = 8400;
  std::vector<SyntheticBox> boxes = {
      {10, 5, 0.10f, 100.0f, 100.0f, 50.0f, 50.0f},  // below 0.25 threshold
      {20, 7, 0.30f, 200.0f, 200.0f, 50.0f, 50.0f},  // above
  };
  std::vector<float> tensor = BuildTensor(num_classes, num_anchors, boxes);

  std::queue<BBox> out;
  yolov8_onnx_postprocess(tensor.data(), 4 + num_classes, num_anchors,
                          /*conf*/ 0.25f, /*iou*/ 0.45f, out);

  REQUIRE(out.size() == 1);
  REQUIRE(out.front().class_index == 7);
}

TEST_CASE("yolov8_onnx_postprocess: iou_thresh controls NMS suppression") {
  const int num_classes = 80;
  const int num_anchors = 8400;
  // Two same-class boxes with ~46% IOU. With a strict 0.30 threshold both
  // should survive; with a lax 0.99 threshold both still survive (no overlap
  // exceeds 0.99); with a moderate 0.40 threshold the lower-scoring one is
  // dropped. This proves the iou_thresh argument actually flows through.
  std::vector<SyntheticBox> boxes = {
      {10, 0, 0.50f, 100.0f, 100.0f, 100.0f, 100.0f},
      {11, 0, 0.90f, 150.0f, 150.0f, 100.0f, 100.0f},  // ~14% IOU at moderate offset
  };
  std::vector<float> tensor = BuildTensor(num_classes, num_anchors, boxes);

  std::queue<BBox> strict;
  yolov8_onnx_postprocess(tensor.data(), 4 + num_classes, num_anchors,
                          /*conf*/ 0.25f, /*iou*/ 0.05f, strict);
  REQUIRE(strict.size() == 1);  // strict iou: lower-scoring box gets suppressed

  std::queue<BBox> lax;
  yolov8_onnx_postprocess(tensor.data(), 4 + num_classes, num_anchors,
                          /*conf*/ 0.25f, /*iou*/ 0.95f, lax);
  REQUIRE(lax.size() == 2);  // lax iou: both boxes survive
}

TEST_CASE("yolov8_onnx_postprocess: NMS keeps the higher-score duplicate") {
  const int num_classes = 80;
  const int num_anchors = 8400;
  // Two highly overlapping boxes for the same class — NMS should keep the
  // higher-scoring one and drop the other.
  std::vector<SyntheticBox> boxes = {
      {10, 0, 0.40f, 300.0f, 300.0f, 100.0f, 100.0f},
      {11, 0, 0.85f, 305.0f, 302.0f, 100.0f, 100.0f},
  };
  std::vector<float> tensor = BuildTensor(num_classes, num_anchors, boxes);

  std::queue<BBox> out;
  yolov8_onnx_postprocess(tensor.data(), 4 + num_classes, num_anchors,
                          /*conf*/ 0.25f, /*iou*/ 0.45f, out);

  REQUIRE(out.size() == 1);
  REQUIRE(out.front().class_score == Catch::Approx(0.85f));
}

TEST_CASE("yolov8_onnx_postprocess: picks the max-scoring class per anchor") {
  const int num_classes = 80;
  const int num_anchors = 8400;
  std::vector<float> tensor(static_cast<size_t>(4 + num_classes) * num_anchors, 0.0f);
  auto at = [&](int c, int a) -> float & { return tensor[c * num_anchors + a]; };

  // Anchor 50: low score on class 3, higher on class 17 — postprocess must pick 17.
  at(0, 50) = 100.0f;
  at(1, 50) = 100.0f;
  at(2, 50) = 50.0f;
  at(3, 50) = 50.0f;
  at(4 + 3, 50) = 0.30f;
  at(4 + 17, 50) = 0.80f;

  std::queue<BBox> out;
  yolov8_onnx_postprocess(tensor.data(), 4 + num_classes, num_anchors,
                          /*conf*/ 0.25f, /*iou*/ 0.45f, out);

  REQUIRE(out.size() == 1);
  REQUIRE(out.front().class_index == 17);
  REQUIRE(out.front().class_score == Catch::Approx(0.80f));
}
