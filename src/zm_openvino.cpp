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

#include "zm_logger.h"
#include "zm_packet.h"

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
                     float confidence) {
  (void)model_file;
  (void)device;
  confidence_ = confidence;
  Warning("OpenVINO::setup: stub — inference not implemented yet");
  return false;
}

OpenVINO::Job *OpenVINO::get_job() {
  Warning("OpenVINO::get_job: stub");
  return nullptr;
}

int OpenVINO::send_image(Job *, Image *) { return -1; }
int OpenVINO::send_frame(Job *, AVFrame *) { return -1; }
int OpenVINO::send_packet(Job *, std::shared_ptr<ZMPacket>) { return -1; }

nlohmann::json OpenVINO::receive_detections(Job *, float) {
  return nlohmann::json::array();
}

#endif  // HAVE_OPENVINO
