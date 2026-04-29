//
// ZoneMinder OpenVINO Class Interface
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

#ifndef ZM_OPENVINO_H
#define ZM_OPENVINO_H

// HAVE_OPENVINO is defined in config.h, which translation units must include
// before this header (matches the zm_mx_accl.h pattern).

#ifdef HAVE_OPENVINO

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <openvino/openvino.hpp>

#include "zm_ffmpeg.h"
#include "zm_object_classes.h"

class Image;
class ZMPacket;

class OpenVINO {
 public:
  // Per-monitor inference handle. Holds the request, scratch frame, and the
  // letterbox scale factors needed to map detections back to the source image.
  class Job {
   public:
    explicit Job(ov::CompiledModel &compiled);
    ~Job();
    Job(const Job &) = delete;
    Job &operator=(const Job &) = delete;

    ov::InferRequest infer_request;
    av_frame_ptr scaled_frame;
    SwsContext *sw_scale_ctx = nullptr;

    // Last source-frame -> model-input scale, used to map detected boxes
    // back to source coordinates. Set in send_frame, read in receive_detections.
    float scale_x = 1.0f;
    float scale_y = 1.0f;
  };

  OpenVINO();
  ~OpenVINO();
  OpenVINO(const OpenVINO &) = delete;
  OpenVINO &operator=(const OpenVINO &) = delete;

  // device: "GPU" / "CPU" / "AUTO" / "" (defaults to GPU then AUTO).
  bool setup(const std::string &model_file,
             const std::string &device = "",
             float confidence = 0.5f);

  Job *get_job();

  int send_image(Job *job, Image *image);
  int send_frame(Job *job, AVFrame *frame);
  int send_packet(Job *job, std::shared_ptr<ZMPacket> packet);

  nlohmann::json receive_detections(Job *job, float object_threshold);

 private:
  ov::Core core_;
  std::shared_ptr<ov::Model> model_;
  ov::CompiledModel compiled_model_;
  std::string device_;

  // Model input geometry (NCHW assumed: 1 x 3 x H x W).
  int input_width_ = 0;
  int input_height_ = 0;

  float confidence_ = 0.5f;

  ObjectClasses object_classes_;

  std::mutex jobs_mutex_;
  std::list<std::unique_ptr<Job>> jobs_;
};

#endif  // HAVE_OPENVINO
#endif  // ZM_OPENVINO_H
