//
// ZoneMinder YOLOv8 ONNX/IR Postprocess Implementation
// Copyright (C) 2026 ZoneMinder Inc
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//

#include "zm_yolov8_onnx_postprocess.h"

void yolov8_onnx_postprocess(
    const float *output,
    int num_channels,
    int num_anchors,
    float confidence_thresh,
    float iou_thresh,
    std::queue<BBox> &out_boxes) {
  if (!output || num_channels < 5 || num_anchors <= 0) return;
  const int num_classes = num_channels - 4;

  const float *cx = output + 0 * num_anchors;
  const float *cy = output + 1 * num_anchors;
  const float *w  = output + 2 * num_anchors;
  const float *h  = output + 3 * num_anchors;
  const float *cls0 = output + 4 * num_anchors;

  for (int a = 0; a < num_anchors; ++a) {
    float best = cls0[a];
    int best_idx = 0;
    for (int c = 1; c < num_classes; ++c) {
      const float s = cls0[c * num_anchors + a];
      if (s > best) {
        best = s;
        best_idx = c;
      }
    }
    if (best < confidence_thresh) continue;

    const float cx_a = cx[a];
    const float cy_a = cy[a];
    const float w_a  = w[a];
    const float h_a  = h[a];

    BBox bbox(best_idx, best,
              cx_a - 0.5f * w_a,
              cy_a - 0.5f * h_a,
              cx_a + 0.5f * w_a,
              cy_a + 0.5f * h_a);
    non_maximum_suppression(out_boxes, bbox, iou_thresh);
  }
}
