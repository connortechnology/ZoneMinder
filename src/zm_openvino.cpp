//
// ZoneMinder OpenVINO Class Implementation
// Copyright (C) 2026 ZoneMinder Inc
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "config.h"

#include "zm_openvino.h"

#ifdef HAVE_OPENVINO

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>

#include "zm_image.h"
#include "zm_logger.h"
#include "zm_packet.h"
#include "zm_yolov8_onnx_postprocess.h"

OpenVINO::Job::Job(ov::CompiledModel &compiled)
    : infer_request(compiled.create_infer_request()),
      scaled_frame(av_frame_ptr(av_frame_alloc())) {
  scaled_frame->format = AV_PIX_FMT_RGB24;
}

OpenVINO::Job::~Job() {
  if (sw_scale_ctx) {
    sws_freeContext(sw_scale_ctx);
    sw_scale_ctx = nullptr;
  }
}

OpenVINO::OpenVINO() = default;

OpenVINO::~OpenVINO() {
  std::lock_guard<std::mutex> lck(jobs_mutex_);
  jobs_.clear();
}

bool OpenVINO::setup(const std::string &model_file,
                     const std::string &device,
                     float confidence,
                     float iou_threshold) {
  confidence_ = confidence;
  iou_threshold_ = iou_threshold;

  Debug(1, "OpenVINO loading %s", model_file.c_str());
  try {
    model_ = core_.read_model(model_file);
  } catch (const std::exception &e) {
    Error("OpenVINO read_model('%s') failed: %s", model_file.c_str(), e.what());
    return false;
  }

  // Standard YOLOv8 export: NCHW, 1 x 3 x H x W. Reject anything else for now.
  ov::PartialShape pshape = model_->input().get_partial_shape();
  if (pshape.rank().is_dynamic() || pshape.size() != 4) {
    Error("OpenVINO: input rank not 4 (got %s)", pshape.to_string().c_str());
    return false;
  }
  if (pshape[2].is_dynamic() || pshape[3].is_dynamic()) {
    Error("OpenVINO: dynamic spatial input not supported (got %s)",
          pshape.to_string().c_str());
    return false;
  }
  input_height_ = static_cast<int>(pshape[2].get_length());
  input_width_  = static_cast<int>(pshape[3].get_length());

  // Pick device: explicit > GPU (if present) > AUTO.
  std::string requested = device;
  if (requested.empty()) {
    std::vector<std::string> available;
    try {
      available = core_.get_available_devices();
    } catch (const std::exception &e) {
      Warning("OpenVINO get_available_devices failed: %s", e.what());
    }
    const bool gpu_available = std::any_of(available.begin(), available.end(),
        [](const std::string &s) { return s.rfind("GPU", 0) == 0; });
    requested = gpu_available ? "GPU" : "AUTO";
    if (!gpu_available) {
      Warning("OpenVINO: GPU plugin not available, falling back to AUTO");
    }
  }
  device_ = requested;

  try {
    compiled_model_ = core_.compile_model(model_, device_);
  } catch (const std::exception &e) {
    Error("OpenVINO compile_model('%s') failed: %s", device_.c_str(), e.what());
    return false;
  }

  Info("OpenVINO ready: model=%s device=%s input=%dx%d",
       model_file.c_str(), device_.c_str(), input_width_, input_height_);
  return true;
}

OpenVINO::Job *OpenVINO::get_job() {
  std::unique_ptr<Job> job;
  try {
    job = std::make_unique<Job>(compiled_model_);
  } catch (const std::exception &e) {
    Error("OpenVINO create_infer_request failed: %s", e.what());
    return nullptr;
  }
  job->scaled_frame->width = input_width_;
  job->scaled_frame->height = input_height_;
  if (av_frame_get_buffer(job->scaled_frame.get(), 32) < 0) {
    Error("OpenVINO: cannot allocate scaled frame buffer");
    return nullptr;
  }

  std::lock_guard<std::mutex> lck(jobs_mutex_);
  Job *raw = job.get();
  jobs_.push_back(std::move(job));
  return raw;
}

int OpenVINO::send_image(Job *job, Image *image) {
  if (!job || !image) return -1;
  av_frame_ptr frame = av_frame_ptr(av_frame_alloc());
  image->PopulateFrame(frame.get());
  return send_frame(job, frame.get());
}

int OpenVINO::send_packet(Job *job, std::shared_ptr<ZMPacket> packet) {
  if (!job || !packet) return -1;
  AVFrame *frame = packet->in_frame.get();
  if (!frame) {
    Error("OpenVINO send_packet: no in_frame in packet %d", packet->image_index);
    return -1;
  }
  return send_frame(job, frame);
}

int OpenVINO::send_frame(Job *job, AVFrame *frame) {
  if (!job || !frame) return -1;

  const SystemTimePoint t0 = std::chrono::system_clock::now();

  // Stretch sws_scale to model input. We deliberately don't letterbox on the
  // first cut — the rescale factors below undo the stretch on detected boxes,
  // and Ultralytics-trained models tolerate aspect-ratio distortion well at
  // common camera ratios. Letterboxing can be added later if accuracy suffers.
  job->sw_scale_ctx = sws_getCachedContext(job->sw_scale_ctx,
      frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
      input_width_, input_height_, AV_PIX_FMT_RGB24,
      SWS_BICUBIC, nullptr, nullptr, nullptr);
  if (!job->sw_scale_ctx) {
    Error("OpenVINO: sws_getCachedContext failed");
    return -1;
  }
  if (sws_scale(job->sw_scale_ctx, frame->data, frame->linesize, 0, frame->height,
                job->scaled_frame->data, job->scaled_frame->linesize) < 0) {
    Error("OpenVINO: sws_scale failed");
    return -1;
  }
  job->scale_x = static_cast<float>(input_width_)  / frame->width;
  job->scale_y = static_cast<float>(input_height_) / frame->height;

  // Build NCHW float input: planar R, G, B in [0, 1].
  ov::Tensor input_tensor(ov::element::f32,
                          {1, 3, static_cast<size_t>(input_height_),
                           static_cast<size_t>(input_width_)});
  float *dst = input_tensor.data<float>();
  const uint8_t *src = job->scaled_frame->data[0];
  const int linesize = job->scaled_frame->linesize[0];
  const int hw = input_height_ * input_width_;
  for (int y = 0; y < input_height_; ++y) {
    const uint8_t *row = src + y * linesize;
    for (int x = 0; x < input_width_; ++x) {
      const uint8_t *p = row + x * 3;
      const int idx = y * input_width_ + x;
      dst[0 * hw + idx] = p[0] * (1.0f / 255.0f);  // R
      dst[1 * hw + idx] = p[1] * (1.0f / 255.0f);  // G
      dst[2 * hw + idx] = p[2] * (1.0f / 255.0f);  // B
    }
  }

  try {
    job->infer_request.set_input_tensor(input_tensor);
    job->infer_request.infer();
  } catch (const std::exception &e) {
    Error("OpenVINO infer failed: %s", e.what());
    return -1;
  }

  const SystemTimePoint t1 = std::chrono::system_clock::now();
  if (t1 - t0 > Milliseconds(200)) {
    Warning("OpenVINO inference slow: %.3fs", FPSeconds(t1 - t0).count());
  } else {
    Debug(1, "OpenVINO inference: %.3fs", FPSeconds(t1 - t0).count());
  }
  return 1;
}

nlohmann::json OpenVINO::receive_detections(Job *job, float object_threshold) {
  nlohmann::json predictions = nlohmann::json::array();
  if (!job) return predictions;

  ov::Tensor out;
  try {
    out = job->infer_request.get_output_tensor();
  } catch (const std::exception &e) {
    Error("OpenVINO get_output_tensor failed: %s", e.what());
    return predictions;
  }

  const ov::Shape shape = out.get_shape();
  if (shape.size() != 3 || shape[0] != 1) {
    Error("OpenVINO: unexpected output rank %zu", shape.size());
    return predictions;
  }
  const int num_channels = static_cast<int>(shape[1]);
  const int num_anchors  = static_cast<int>(shape[2]);

  std::queue<BBox> bboxes;
  yolov8_onnx_postprocess(out.data<const float>(), num_channels, num_anchors,
                          object_threshold, iou_threshold_, bboxes);

  while (!bboxes.empty()) {
    BBox b = bboxes.front();
    bboxes.pop();
    // Map model-input coords back to source frame.
    const float l = b.x_min / job->scale_x;
    const float t = b.y_min / job->scale_y;
    const float r = b.x_max / job->scale_x;
    const float bot = b.y_max / job->scale_y;
    std::array<float, 4> cv_bbox = {l, t, r, bot};
    const std::string &class_name = object_classes_.getClassName(b.class_index);
    predictions.push_back({{"class", class_name},
                           {"bbox", cv_bbox},
                           {"score", b.class_score}});
  }
  return predictions;
}

#endif  // HAVE_OPENVINO
