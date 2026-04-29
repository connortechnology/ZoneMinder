//
// ZoneMinder YOLOv8 ONNX/IR Postprocess
// Copyright (C) 2026 ZoneMinder Inc
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//

#ifndef ZM_YOLOV8_ONNX_POSTPROCESS_H
#define ZM_YOLOV8_ONNX_POSTPROCESS_H

#include <queue>

#include "zm_memx_yolo_core.h"  // BBox, non_maximum_suppression

// Decode the standard Ultralytics YOLOv8 ONNX/IR output to BBoxes.
//
// Tensor layout: channel-major [4 + num_classes, num_anchors] (after dropping
// the leading batch dim of 1). For COCO this is [84, 8400]: channels 0..3 are
// cx,cy,w,h in model-input pixels (anchor-free, no sigmoid on coords); the
// remaining channels are per-class scores already passed through sigmoid by
// the model graph.
//
// Output boxes are in model-input coordinate space — caller scales back.
void yolov8_onnx_postprocess(
    const float *output,
    int num_channels,
    int num_anchors,
    float confidence_thresh,
    float iou_thresh,
    std::queue<BBox> &out_boxes);

#endif
