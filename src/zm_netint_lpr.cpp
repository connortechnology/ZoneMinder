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

#include "config.h"

#include "zm_netint_lpr.h"

#include <cmath>
#include <cstdint>
#include <fstream>

/* ------------------------------------------------------------------------- *
 * Hardware-independent postprocess. Kept outside HAVE_QUADRA so it stays
 * buildable and unit testable without a card or libxcoder.
 * ------------------------------------------------------------------------- */

namespace zm_lpr {

std::vector<std::string> utf8_codepoints(const std::string &text) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    size_t len = 1;
    if ((lead & 0x80) == 0x00) len = 1;
    else if ((lead & 0xE0) == 0xC0) len = 2;
    else if ((lead & 0xF0) == 0xE0) len = 3;
    else if ((lead & 0xF8) == 0xF0) len = 4;
    // A stray continuation byte would give len 1, which at least makes progress
    // rather than looping forever on malformed input.
    if (i + len > text.size()) len = text.size() - i;
    out.emplace_back(text, i, len);
    i += len;
  }
  return out;
}

const std::vector<std::string> &default_charset() {
  // The charset the bundled reclicense model was trained on (CCPD): index 0 is
  // the CTC blank, then the province glyphs, then digits and A-Z less I and O.
  // Stored as one literal and split by codepoint so the ordering cannot drift
  // from the reference through hand transcription.
  static const std::vector<std::string> charset = utf8_codepoints(
      "#京沪津渝冀晋蒙辽吉黑苏浙皖闽赣鲁豫鄂湘粤桂琼川贵云藏陕甘青宁新学警港澳挂使领民航危"
      "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ险品");
  return charset;
}

std::vector<std::string> load_charset(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return default_charset();
  }

  std::vector<std::string> charset;
  std::string line;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();  // tolerate CRLF-terminated files
    }
    charset.push_back(line);
  }

  if (charset.empty()) {
    return default_charset();
  }
  return charset;
}

std::string ctc_decode(const float *tensor, int chars_num, int max_chars,
                       const std::vector<std::string> &charset) {
  std::string out;
  if (!tensor || chars_num <= 0 || max_chars <= 0) {
    return out;
  }

  int prev = -1;
  for (int n = 0; n < max_chars; n++) {
    const float *step = tensor + static_cast<size_t>(n) * chars_num;

    int best = 0;
    for (int k = 1; k < chars_num; k++) {
      if (step[k] > step[best]) {
        best = k;
      }
    }

    // Collapse runs of the same class. Comparing against the previous *raw*
    // class (blank included) is what lets a blank separate a doubled character.
    if (best == prev) {
      continue;
    }
    prev = best;

    if (best == 0) {
      continue;  // CTC blank
    }
    if (best < static_cast<int>(charset.size())) {
      out += charset[best];
    }
    // A class with no charset entry is dropped rather than read out of bounds:
    // it means the charset file does not match the model.
  }
  return out;
}

void order_landmarks(const double in[4][2], double out[4][2]) {
  // Corners by extremes of the diagonals: (x+y) is smallest top-left and
  // largest bottom-right, (y-x) is smallest top-right and largest bottom-left.
  // This holds under rotation, which a plate seen off-axis will have.
  int tl = 0, tr = 0, bl = 0, br = 0;
  for (int j = 1; j < 4; j++) {
    if (in[tl][0] + in[tl][1] > in[j][0] + in[j][1]) tl = j;
    if (in[br][0] + in[br][1] < in[j][0] + in[j][1]) br = j;
    if (in[tr][1] - in[tr][0] > in[j][1] - in[j][0]) tr = j;
    if (in[bl][1] - in[bl][0] < in[j][1] - in[j][0]) bl = j;
  }

  out[0][0] = in[tl][0];  out[0][1] = in[tl][1];
  out[1][0] = in[tr][0];  out[1][1] = in[tr][1];
  out[2][0] = in[bl][0];  out[2][1] = in[bl][1];
  out[3][0] = in[br][0];  out[3][1] = in[br][1];
}

void perspective_coeffs(const double l[4][2], int out_w, int out_h, double f[9]) {
  // Inverse mapping of the unit destination rect onto the source quad. See
  // "Spatial Transformations", inverse perspective section; this matches
  // ffmpeg's vf_perspective, which the NetInt sample also borrows from.
  const double w = out_w;
  const double h = out_h;

  const double dx3 = l[0][0] - l[1][0] - l[2][0] + l[3][0];
  const double dy3 = l[0][1] - l[1][1] - l[2][1] + l[3][1];

  f[6] = (dx3 * (l[2][1] - l[3][1]) - dy3 * (l[2][0] - l[3][0])) * h;
  f[7] = (dy3 * (l[1][0] - l[3][0]) - dx3 * (l[1][1] - l[3][1])) * w;

  const double q = (l[1][0] - l[3][0]) * (l[2][1] - l[3][1]) -
                   (l[2][0] - l[3][0]) * (l[1][1] - l[3][1]);

  f[0] = q * (l[1][0] - l[0][0]) * h + f[6] * l[1][0];
  f[1] = q * (l[2][0] - l[0][0]) * w + f[7] * l[2][0];
  f[2] = q * l[0][0] * w * h;
  f[3] = q * (l[1][1] - l[0][1]) * h + f[6] * l[1][1];
  f[4] = q * (l[2][1] - l[0][1]) * w + f[7] * l[2][1];
  f[5] = q * l[0][1] * w * h;
  f[8] = q * w * h;
}

std::string ascii_label(const std::string &utf8) {
  std::string out;
  for (const std::string &cp : utf8_codepoints(utf8)) {
    // Single-byte codepoints below 0x80 are what the bitmap font is indexed by;
    // everything else collapses to one '?' rather than a run of blank glyphs.
    if (cp.size() == 1 && static_cast<unsigned char>(cp[0]) < 0x80) {
      out += cp;
    } else {
      out += '?';
    }
  }
  return out;
}

}  // namespace zm_lpr

#if HAVE_QUADRA

#include "zm_image.h"
#include "zm_logger.h"
#include "zm_monitor.h"
#include "zm_packet.h"
#include "zm_vector2.h"

#include "ni_yolo_utils.h"

#include <algorithm>

namespace {

/* Anchor layout of the bundled plate detector. It is a YOLOv5 head with 3
 * output layers of 3 anchors each, so 9 anchor boxes / 18 bias values.
 */
int g_masks[3][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}};
int g_sequence[3] = {0, 1, 2};
float g_biases[18] = {4, 5, 8, 10, 13, 16, 23, 29, 43, 55, 73, 105, 146, 217, 231, 300, 335, 433};

// Per plate the head regresses 4 landmarks (x,y each) on top of the usual
// box/objectness/class channels, so the channel stride carries 8 extra entries
// and the class scores start after them.
constexpr int kLandmarkEntries = 8;
constexpr int kPlateClasses = 2;
constexpr int kObjEntry = 4;
constexpr int kLandmarkEntry = 5;
constexpr int kClassEntry = kLandmarkEntry + kLandmarkEntries;  // 13

// Normalisation the recogniser was trained with.
constexpr float kRecMean = 0.588f;
constexpr float kRecStd = 0.193f;

/* Bilinear sample of a plane at a 1/256th-pixel fixed-point coordinate,
 * clamping at the edges. Lifted from the NetInt sample, which in turn takes it
 * from ffmpeg's vf_perspective.
 */
uint8_t linear_interpolate(const uint8_t *data, int x, int y, int width, int height,
                           int linesize) {
  int sum;
  int subU = x & 255;
  int subV = y & 255;
  x >>= 8;
  y >>= 8;

  int index = x + y * linesize;
  const int subUI = 256 - subU;
  const int subVI = 256 - subV;

  if (static_cast<unsigned>(x) < static_cast<unsigned>(width - 1)) {
    if (static_cast<unsigned>(y) < static_cast<unsigned>(height - 1)) {
      sum = subVI * (subUI * data[index] + subU * data[index + 1]) +
            subV * (subUI * data[index + linesize] + subU * data[index + linesize + 1]);
      sum = (sum + (1 << 15)) >> 16;
    } else {
      y = (y < 0) ? 0 : height - 1;
      index = x + y * linesize;
      sum = subUI * data[index] + subU * data[index + 1];
      sum = (sum + (1 << 7)) >> 8;
    }
  } else {
    x = (x < 0) ? 0 : width - 1;
    if (static_cast<unsigned>(y) < static_cast<unsigned>(height - 1)) {
      index = x + y * linesize;
      sum = subVI * data[index] + subV * data[index + linesize];
      sum = (sum + (1 << 7)) >> 8;
    } else {
      y = (y < 0) ? 0 : height - 1;
      index = x + y * linesize;
      sum = data[index];
    }
  }

  return static_cast<uint8_t>(std::clamp(sum, 0, 255));
}

/* Resample one plane through the perspective coefficients. hsub/vsub are the
 * chroma subsampling shifts, so the same coefficients drive Y and the half-size
 * U/V planes.
 */
void linear_sample(uint8_t *dst, const uint8_t *src, int dst_linesize, int src_linesize,
                   int hsub, int vsub, int dst_w, int dst_h, int src_w, int src_h,
                   const double f[9]) {
  for (int y = 0; y < dst_h; y++) {
    const int sy = y << vsub;
    for (int x = 0; x < dst_w; x++) {
      const int sx = x << hsub;
      const double denom = f[6] * sx + f[7] * sy + f[8];
      if (denom == 0.0) {
        dst[x + y * dst_linesize] = 0;
        continue;
      }
      const int u = static_cast<int>(lrint(256 * (f[0] * sx + f[1] * sy + f[2]) / denom)) >> hsub;
      const int v = static_cast<int>(lrint(256 * (f[3] * sx + f[4] * sy + f[5]) / denom)) >> vsub;
      dst[x + y * dst_linesize] = linear_interpolate(src, u, v, src_w, src_h, src_linesize);
    }
  }
}

/* Read the 4 landmarks a detection regressed, in detector-input coordinates,
 * then scale them back to source-image coordinates.
 */
void decode_landmarks(const YoloModelCtx *ctx, const detection *det, double landmarks[4][2],
                      float gain_x, float gain_y) {
  const ni_roi_network_layer_t *l = &ctx->layers[det->layer_idx];
  const int row = det->sub_idx / l->width;
  const int col = det->sub_idx % l->width;
  const int stride = l->width * l->height;
  const int n = l->mask[det->color];
  const int index = entry_index(const_cast<ni_roi_network_layer_t *>(l), 0, det->color,
                                det->sub_idx, kLandmarkEntry);
  const float *x = l->output;

  for (int i = 0; i < 4; i++) {
    landmarks[i][0] = (x[index + (2 * i) * stride] * l->biases[2 * n] +
                       static_cast<float>(col) * anchor_stride[l->index]) / gain_x;
    landmarks[i][1] = (x[index + (2 * i + 1) * stride] * l->biases[2 * n + 1] +
                       static_cast<float>(row) * anchor_stride[l->index]) / gain_y;
  }
}

}  // namespace

Quadra_LPR::Quadra_LPR(Monitor *p_monitor, bool p_use_hwframe) :
  monitor(p_monitor),
  dec_stream(nullptr),
  dec_ctx(nullptr),
  det_network(nullptr),
  det_model(),
  det_frame(),
  det_scale_ctx(nullptr),
  det_width(0),
  det_height(0),
  rec_network(nullptr),
  rec_frame(),
  rec_scale_ctx(nullptr),
  rec_width(0),
  rec_height(0),
  chars_num(0),
  max_chars(0),
  obj_thresh(0.25),
  nms_thresh(0.45),
  use_hwframe(p_use_hwframe),
  models_created(false),
  draw_annotations(true)
{
  obj_thresh = monitor->ObjectDetection_Object_Threshold();
  nms_thresh = monitor->ObjectDetection_NMS_Threshold();
}

Quadra_LPR::~Quadra_LPR() {
  destroy_det_model();

  if (det_scale_ctx) {
    sws_freeContext(det_scale_ctx);
    det_scale_ctx = nullptr;
  }
  if (rec_scale_ctx) {
    sws_freeContext(rec_scale_ctx);
    rec_scale_ctx = nullptr;
  }

  if (det_network) {
    ni_frame_buffer_free(&det_frame.api_frame.data.frame);
    ni_packet_buffer_free(&det_frame.api_packet.data.packet);
    ni_cleanup_network_context(det_network, false);
    det_network = nullptr;
  }
  if (rec_network) {
    ni_frame_buffer_free(&rec_frame.api_frame.data.frame);
    ni_packet_buffer_free(&rec_frame.api_packet.data.packet);
    ni_cleanup_network_context(rec_network, false);
    rec_network = nullptr;
  }
}

void Quadra_LPR::destroy_det_model() {
  if (!models_created) {
    return;
  }
  if (det_model.out_tensor) {
    for (int i = 0; i < det_model.output_number; i++) {
      free(det_model.out_tensor[i]);
    }
    free(det_model.out_tensor);
    det_model.out_tensor = nullptr;
  }
  if (det_model.layers) {
    for (int i = 0; i < det_model.output_number; i++) {
      free(det_model.layers[i].biases);
    }
    free(det_model.layers);
    det_model.layers = nullptr;
  }
  free(det_model.det_cache.dets);
  det_model.det_cache.dets = nullptr;
  models_created = false;
}

bool Quadra_LPR::create_det_model(ni_network_data_t *network_data) {
  det_model.obj_thresh = obj_thresh;
  det_model.nms_thresh = nms_thresh;
  det_model.input_width = det_width;
  det_model.input_height = det_height;
  det_model.output_number = network_data->output_num;

  if (network_data->output_num > 3) {
    Error("LPR detector has %d output layers, expected at most 3", network_data->output_num);
    return false;
  }

  det_model.out_tensor =
      static_cast<uint8_t **>(calloc(network_data->output_num, sizeof(uint8_t *)));
  if (!det_model.out_tensor) {
    Error("LPR: cannot allocate detector output tensor table");
    return false;
  }
  models_created = true;  // from here on destroy_det_model() must run

  for (uint32_t i = 0; i < network_data->output_num; i++) {
    ni_network_layer_params_t *p_param = &network_data->linfo.out_param[i];
    det_model.out_tensor[i] =
        static_cast<uint8_t *>(malloc(ni_ai_network_layer_dims(p_param) * sizeof(float)));
    if (!det_model.out_tensor[i]) {
      Error("LPR: cannot allocate detector output tensor %d", i);
      return false;
    }
  }

  det_model.layers = static_cast<ni_roi_network_layer_t *>(
      malloc(sizeof(ni_roi_network_layer_t) * network_data->output_num));
  if (!det_model.layers) {
    Error("LPR: cannot allocate detector layers");
    return false;
  }
  memset(det_model.layers, 0, sizeof(ni_roi_network_layer_t) * network_data->output_num);

  for (uint32_t i = 0; i < network_data->output_num; i++) {
    ni_roi_network_layer_t *layer = &det_model.layers[i];
    layer->index = static_cast<int32_t>(i);
    layer->width = network_data->linfo.out_param[i].sizes[0];
    layer->height = network_data->linfo.out_param[i].sizes[1];
    layer->channel = network_data->linfo.out_param[i].sizes[2];
    layer->component = 3;
    layer->classes = kPlateClasses;
    layer->padding = kLandmarkEntries;
    layer->output = reinterpret_cast<float *>(det_model.out_tensor[i]);
    memcpy(layer->mask, &g_masks[i][0], sizeof(layer->mask));

    layer->biases = static_cast<float *>(malloc(sizeof(g_biases)));
    if (!layer->biases) {
      Error("LPR: cannot allocate detector layer biases");
      return false;
    }
    memcpy(layer->biases, &g_biases[0], sizeof(g_biases));

    Debug(2, "LPR detector layer %d: %dx%d, ch %d, classes %d, padding %d",
          i, layer->width, layer->height, layer->channel, layer->classes, layer->padding);
  }

  det_model.entry_set.obj_entry = kObjEntry;
  det_model.entry_set.class_entry = kClassEntry;
  det_model.entry_set.coods_entry = 0;

  det_model.det_cache.dets_num = 0;
  det_model.det_cache.capacity = 20;
  det_model.det_cache.dets =
      static_cast<detection *>(malloc(sizeof(detection) * det_model.det_cache.capacity));
  if (!det_model.det_cache.dets) {
    Error("LPR: cannot allocate detection cache");
    return false;
  }

  return true;
}

bool Quadra_LPR::setup(
    AVStream *p_dec_stream,
    AVCodecContext *p_dec_ctx,
    const std::string &det_nbg_file,
    const std::string &rec_nbg_file,
    int deviceid)
{
  dec_stream = p_dec_stream;
  dec_ctx = p_dec_ctx;

  Debug(1, "LPR setup: detector %s, recogniser %s, device %d",
        det_nbg_file.c_str(), rec_nbg_file.c_str(), deviceid);

  /* Both stages run in software-frame mode regardless of how the frame arrived:
   * the perspective warp between them needs direct pixel access, so there is
   * nothing to gain from keeping the frame on the card.
   */
  int ret = ni_alloc_network_context(&det_network, false, deviceid, 30,
                                     GC620_RGB888_PLANAR, 0, 0, det_nbg_file.c_str());
  if (ret != 0) {
    Error("LPR: failed to allocate detector network context on card %d", deviceid);
    return false;
  }

  det_width = static_cast<int>(det_network->network_data.linfo.in_param[0].sizes[0]);
  det_height = static_cast<int>(det_network->network_data.linfo.in_param[0].sizes[1]);
  Debug(1, "LPR detector input %dx%d, %d output layers",
        det_width, det_height, det_network->network_data.output_num);

  if (!create_det_model(&det_network->network_data)) {
    return false;
  }

  det_frame.scale_width = det_width;
  det_frame.scale_height = det_height;
  det_frame.scale_format = GC620_RGB888_PLANAR;

  ret = ni_ai_packet_buffer_alloc(&det_frame.api_packet.data.packet, &det_network->network_data);
  if (ret != NI_RETCODE_SUCCESS) {
    Error("LPR: failed to allocate detector packet buffer");
    return false;
  }
  ret = ni_ai_frame_buffer_alloc(&det_frame.api_frame.data.frame, &det_network->network_data);
  if (ret != NI_RETCODE_SUCCESS) {
    Error("LPR: failed to allocate detector frame buffer");
    return false;
  }

  /* Stage 2: recogniser. */
  ret = ni_alloc_network_context(&rec_network, false, deviceid, 30,
                                 GC620_RGB888_PLANAR, 0, 0, rec_nbg_file.c_str());
  if (ret != 0) {
    Error("LPR: failed to allocate recogniser network context on card %d", deviceid);
    return false;
  }

  rec_width = static_cast<int>(rec_network->network_data.linfo.in_param[0].sizes[0]);
  rec_height = static_cast<int>(rec_network->network_data.linfo.in_param[0].sizes[1]);
  chars_num = static_cast<int>(rec_network->network_data.linfo.out_param[0].sizes[0]);
  max_chars = static_cast<int>(rec_network->network_data.linfo.out_param[0].sizes[1]);
  Debug(1, "LPR recogniser input %dx%d, %d classes over %d timesteps",
        rec_width, rec_height, chars_num, max_chars);

  if (rec_width <= 0 || rec_height <= 0 || chars_num <= 0 || max_chars <= 0) {
    Error("LPR: recogniser reported nonsensical dimensions");
    return false;
  }

  ret = ni_ai_packet_buffer_alloc(&rec_frame.api_packet.data.packet, &rec_network->network_data);
  if (ret != NI_RETCODE_SUCCESS) {
    Error("LPR: failed to allocate recogniser packet buffer");
    return false;
  }
  ret = ni_ai_frame_buffer_alloc(&rec_frame.api_frame.data.frame, &rec_network->network_data);
  if (ret != NI_RETCODE_SUCCESS) {
    Error("LPR: failed to allocate recogniser frame buffer");
    return false;
  }

  rec_tensor.resize(static_cast<size_t>(chars_num) * max_chars);
  rec_planar.resize(static_cast<size_t>(rec_width) * rec_height * 3);

  persp_frame = av_frame_ptr{av_frame_alloc()};
  persp_frame->width = rec_width;
  persp_frame->height = rec_height;
  persp_frame->format = AV_PIX_FMT_YUV420P;
  if (av_frame_get_buffer(persp_frame.get(), 32)) {
    Error("LPR: cannot allocate perspective frame");
    return false;
  }

  rgb_frame = av_frame_ptr{av_frame_alloc()};
  rgb_frame->width = rec_width;
  rgb_frame->height = rec_height;
  rgb_frame->format = AV_PIX_FMT_GBRP;
  if (av_frame_get_buffer(rgb_frame.get(), 32)) {
    Error("LPR: cannot allocate rgb frame");
    return false;
  }

  rec_scale_ctx = sws_getContext(rec_width, rec_height, AV_PIX_FMT_YUV420P,
                                 rec_width, rec_height, AV_PIX_FMT_GBRP,
                                 SWS_BICUBIC, nullptr, nullptr, nullptr);
  if (!rec_scale_ctx) {
    Error("LPR: cannot create recogniser scale context");
    return false;
  }

  /* Charset alongside the recogniser model, e.g. reclicense_fp16.nb ->
   * reclicense_fp16.charset, falling back to plates.charset in the same
   * directory and then to the built-in set. This mirrors how .names files sit
   * beside detection models.
   */
  std::string charset_file = rec_nbg_file;
  const size_t dot_pos = charset_file.rfind('.');
  charset_file = (dot_pos == std::string::npos) ? charset_file + ".charset"
                                                : charset_file.substr(0, dot_pos) + ".charset";
  if (!std::ifstream(charset_file).good()) {
    const size_t slash_pos = rec_nbg_file.rfind('/');
    if (slash_pos != std::string::npos) {
      charset_file = rec_nbg_file.substr(0, slash_pos + 1) + "plates.charset";
    }
  }
  charset = zm_lpr::load_charset(charset_file);
  if (charset == zm_lpr::default_charset()) {
    Debug(1, "LPR: no charset file for %s, using the built-in CCPD charset. "
             "Recognition of non-Chinese plates will be poor until the recogniser "
             "is retrained and a matching .charset supplied.",
          rec_nbg_file.c_str());
  } else {
    Debug(1, "LPR: loaded %zu charset entries from %s", charset.size(), charset_file.c_str());
  }

  if (static_cast<int>(charset.size()) != chars_num) {
    Warning("LPR: charset has %zu entries but the recogniser emits %d classes; "
            "decoded text will be wrong if these do not correspond",
            charset.size(), chars_num);
  }

  return true;
}

int Quadra_LPR::generate_det_frame(ni_session_data_io_t *ai_frame, AVFrame *avframe) {
  if (!det_scale_ctx) {
    det_scale_ctx = sws_getContext(
        avframe->width, avframe->height, static_cast<AVPixelFormat>(avframe->format),
        det_width, det_height, AV_PIX_FMT_GBRP, SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!det_scale_ctx) {
      Error("LPR: cannot create detector scale context for %dx%d fmt %d",
            avframe->width, avframe->height, avframe->format);
      return -1;
    }
    Debug(1, "LPR: scaling %dx%d fmt %d -> %dx%d planar BGR",
          avframe->width, avframe->height, avframe->format, det_width, det_height);
  }

  /* The network wants planar BGR. swscale gives GBRP, whose plane order is
   * G,B,R, so point each output plane at the right third of the tensor buffer
   * rather than shuffling afterwards.
   */
  const size_t plane = static_cast<size_t>(det_width) * det_height;
  uint8_t *base = ai_frame->data.frame.p_data[0];
  uint8_t *dst_data[3] = {base + plane, base, base + plane * 2};
  int dst_stride[3] = {det_width, det_width, det_width};

  const int ret = sws_scale(det_scale_ctx, avframe->data, avframe->linesize, 0,
                            avframe->height, dst_data, dst_stride);
  if (ret < 0) {
    Error("LPR: cannot scale detector input frame");
    return ret;
  }
  return 0;
}

int Quadra_LPR::get_plate_boxes(int img_width, int img_height, std::vector<PlateBox> &plates) {
  const int ret = ni_get_yolov5_detections(&det_model, g_sequence, 0);
  if (ret < 0) {
    Error("LPR: cannot get plate detections");
    return ret;
  }
  if (ret == 0) {
    return 0;
  }

  const int dets_num = ret;
  detection *dets = det_model.det_cache.dets;

  // The detector consumes a plain resize of the whole frame (no letterboxing),
  // so coordinates scale back independently on each axis.
  const float gain_x = det_model.input_width / static_cast<float>(img_width);
  const float gain_y = det_model.input_height / static_cast<float>(img_height);

  for (int i = 0; i < dets_num; i++) {
    if (dets[i].max_prob == 0) {
      continue;  // suppressed by NMS
    }
    PlateBox plate = {};
    ni_resize_coords_tiling_mode(&dets[i], &plate.box, img_width, img_height, gain_x, gain_y);

    double raw[4][2];
    decode_landmarks(&det_model, &dets[i], raw, gain_x, gain_y);
    zm_lpr::order_landmarks(raw, plate.landmark);

    plate.box.prob = dets[i].max_prob;
    plates.push_back(plate);
  }

  return static_cast<int>(plates.size());
}

int Quadra_LPR::run_detector(AVFrame *avframe, std::vector<PlateBox> &plates) {
  int ret = generate_det_frame(&det_frame.api_frame, avframe);
  if (ret < 0) {
    return ret;
  }

  ret = ni_set_network_input(det_network, false, &det_frame.api_frame, nullptr,
                             avframe->width, avframe->height, &det_frame, true);
  if (ret == NIERROR(EAGAIN)) {
    Debug(2, "LPR: detector busy, skipping frame");
    return 0;
  }
  if (ret != 0) {
    Error("LPR: cannot feed the plate detector");
    return -1;
  }

  ret = ni_get_network_output(det_network, false, &det_frame, true /* blockable */,
                              true /* convert */, det_model.out_tensor);
  if (ret == NIERROR(EAGAIN)) {
    return 0;
  }
  if (ret != 0) {
    Error("LPR: error reading plate detector output (%d)", ret);
    return -1;
  }

  return get_plate_boxes(avframe->width, avframe->height, plates);
}

void Quadra_LPR::sample_perspective(const AVFrame *in, AVFrame *out, const double landmark[4][2]) {
  double f[9];
  zm_lpr::perspective_coeffs(landmark, out->width, out->height, f);

  linear_sample(out->data[0], in->data[0], out->linesize[0], in->linesize[0],
                0, 0, out->width, out->height, in->width, in->height, f);

  /* Chroma planes are half resolution on both axes, hence the 1,1 shifts, which
   * also bring the sampled coordinates into chroma space.
   *
   * The source bounds passed here are the chroma plane's own dimensions. NetInt's
   * sample passes the full luma dimensions, which makes linear_interpolate's edge
   * clamp compare a chroma-space coordinate against a luma-space limit: the clamp
   * then never fires, and sampling the bottom-right of a plate indexes past the
   * end of the chroma plane. Using the real plane size both fixes the edge
   * handling and keeps the read in bounds.
   */
  const int chroma_w = (in->width + 1) / 2;
  const int chroma_h = (in->height + 1) / 2;
  linear_sample(out->data[1], in->data[1], out->linesize[1], in->linesize[1],
                1, 1, out->width / 2, out->height / 2, chroma_w, chroma_h, f);
  linear_sample(out->data[2], in->data[2], out->linesize[2], in->linesize[2],
                1, 1, out->width / 2, out->height / 2, chroma_w, chroma_h, f);
}

int Quadra_LPR::recognise_plate(AVFrame *avframe, const PlateBox &plate, std::string &text) {
  sample_perspective(avframe, persp_frame.get(), plate.landmark);

  int ret = sws_scale(rec_scale_ctx, persp_frame->data, persp_frame->linesize, 0,
                      persp_frame->height, rgb_frame->data, rgb_frame->linesize);
  if (ret < 0) {
    Error("LPR: cannot convert the deskewed plate to planar RGB");
    return -1;
  }

  /* Normalise into planar float in B,G,R order. GBRP planes are G,B,R. */
  const uint8_t *gdata = rgb_frame->data[0];
  const uint8_t *bdata = rgb_frame->data[1];
  const uint8_t *rdata = rgb_frame->data[2];
  const size_t plane = static_cast<size_t>(rec_width) * rec_height;
  float *bp = rec_planar.data();
  float *gp = rec_planar.data() + plane;
  float *rp = rec_planar.data() + plane * 2;

  for (int y = 0, k = 0; y < rec_height; y++) {
    const uint8_t *grow = gdata + static_cast<size_t>(y) * rgb_frame->linesize[0];
    const uint8_t *brow = bdata + static_cast<size_t>(y) * rgb_frame->linesize[1];
    const uint8_t *rrow = rdata + static_cast<size_t>(y) * rgb_frame->linesize[2];
    for (int x = 0; x < rec_width; x++, k++) {
      bp[k] = (brow[x] / 255.0f - kRecMean) / kRecStd;
      gp[k] = (grow[x] / 255.0f - kRecMean) / kRecStd;
      rp[k] = (rrow[x] / 255.0f - kRecMean) / kRecStd;
    }
  }

  ni_network_layer_params_t *in_param = &rec_network->network_data.linfo.in_param[0];
  ni_network_convert_tensor_to_data(
      rec_frame.api_frame.data.frame.p_data[0], ni_ai_network_layer_size(in_param),
      rec_planar.data(), rec_planar.size() * sizeof(float), in_param);

  ret = ni_set_network_input(rec_network, false, &rec_frame.api_frame, nullptr,
                             rec_width, rec_height, &rec_frame, true);
  if (ret == NIERROR(EAGAIN)) {
    Debug(2, "LPR: recogniser busy, skipping plate");
    return 0;
  }
  if (ret != 0) {
    Error("LPR: cannot feed the plate recogniser");
    return -1;
  }

  uint8_t *out_tensor[1] = {reinterpret_cast<uint8_t *>(rec_tensor.data())};
  ret = ni_get_network_output(rec_network, false, &rec_frame, true /* blockable */,
                              true /* convert */, out_tensor);
  if (ret == NIERROR(EAGAIN)) {
    return 0;
  }
  if (ret != 0) {
    Error("LPR: error reading recogniser output (%d)", ret);
    return -1;
  }

  text = zm_lpr::ctc_decode(rec_tensor.data(), chars_num, max_chars, charset);
  return text.empty() ? 0 : 1;
}

void Quadra_LPR::annotate(AVFrame *frame, const std::vector<RecognisedPlate> &plates) {
  // Wraps the frame's buffer directly, so the drawing lands on the frame itself
  // rather than a copy - the same thing Quadra_Yolo relies on.
  Image image(frame);
  const int label_size = monitor->LabelSize();
  const int line_width = std::max(1, label_size);

  for (const RecognisedPlate &plate : plates) {
    const Rgb colour = kRGBRed;
    for (int i = 0; i < line_width; i++) {
      image.DrawBox(plate.box.left + i, plate.box.top + i,
                    plate.box.right - 2 * i, plate.box.bottom - 2 * i, colour);
    }

    /* Put the label above the box where there is room, otherwise just below the
     * top edge, so a plate near the top of the frame does not lose its number
     * off-screen. Annotate clamps to the image, but clamping alone would drop it
     * on top of the box.
     */
    const int text_height = 8 * label_size;
    int label_y = plate.box.top - text_height - 1;
    if (label_y < 0) label_y = plate.box.top + 1;

    image.Annotate(zm_lpr::ascii_label(plate.text),
                   Vector2(plate.box.left, label_y),
                   label_size, kRGBWhite, kRGBBlack);
  }
}

AVFrame *Quadra_LPR::software_frame(const std::shared_ptr<ZMPacket> &packet, bool *owned) {
  *owned = false;

  // A decoded software frame is always preferable: no download, no filter.
  if (packet->in_frame) {
    return packet->in_frame.get();
  }
  if (!packet->hw_frame) {
    return nullptr;
  }

  AVFrame *hw = packet->hw_frame.get();
  if (!hwdl_filter.initialised &&
      !hwdl_filter.setup("hwdownload,format=yuv420p", "", dec_ctx, dec_stream->time_base,
                         hw->hw_frames_ctx, dec_ctx->pix_fmt)) {
    Error("LPR: cannot set up hwdownload; plate recognition needs pixel access");
    return nullptr;
  }

  AVFrame *sw = nullptr;
  if (hwdl_filter.execute(hw, &sw) < 0 || !sw) {
    Error("LPR: cannot download frame from the card");
    return nullptr;
  }
  *owned = true;
  return sw;
}

int Quadra_LPR::detect(const std::shared_ptr<ZMPacket> &packet) {
  if (!det_network || !rec_network) {
    return -1;
  }

  bool owned = false;
  AVFrame *avframe = software_frame(packet, &owned);
  if (!avframe) {
    return 0;
  }

  // Free a downloaded frame on every exit path.
  struct FrameGuard {
    AVFrame *frame;
    bool owned;
    ~FrameGuard() { if (owned && frame) av_frame_free(&frame); }
  } guard{avframe, owned};

  std::vector<PlateBox> plates;
  int ret = run_detector(avframe, plates);
  if (ret < 0) {
    return ret;
  }
  if (plates.empty()) {
    Debug(2, "LPR: no plates in frame %d", packet->image_index);
    return 0;
  }

  Debug(1, "LPR: %zu plate(s) detected in frame %d", plates.size(), packet->image_index);

  std::vector<RecognisedPlate> recognised_plates;
  int recognised = 0;
  for (const PlateBox &plate : plates) {
    std::string text;
    ret = recognise_plate(avframe, plate, text);
    if (ret < 0) {
      return ret;
    }
    if (ret == 0) {
      continue;  // nothing legible on this plate
    }
    recognised_plates.push_back({plate.box, text});

    Debug(1, "LPR: plate '%s' at (%d,%d)-(%d,%d) prob %.2f",
          text.c_str(), plate.box.left, plate.box.top, plate.box.right, plate.box.bottom,
          plate.box.prob);

    if (!packet->detections.is_array()) {
      packet->detections = nlohmann::json::array();
    }
    packet->detections.push_back({
        {"class", "plate"},
        {"bbox", std::array<int, 4>{plate.box.left, plate.box.top,
                                    plate.box.right, plate.box.bottom}},
        {"score", plate.box.prob},
        {"text", text},
    });
    recognised++;
  }

  if (draw_annotations && !recognised_plates.empty()) {
    /* Prefer a frame an earlier stage has already annotated, so yolo boxes and
     * plate boxes end up on the same image rather than one replacing the other.
     */
    AVFrame *target = packet->ai_frame ? packet->ai_frame.get() : avframe;
    annotate(target, recognised_plates);

    if (!packet->ai_frame) {
      /* Nothing has published an ai frame for this packet, so publish ours -
       * otherwise the drawing we just did is never shown. When the frame came
       * from a hardware download we already own it and can hand it straight
       * over; when it is the decoded in_frame we pass a new reference to the
       * same buffer, which is what makes the annotation visible in both.
       */
      if (owned) {
        packet->set_ai_frame(avframe);
        guard.owned = false;  // ownership transferred to the packet
      } else {
        AVFrame *ref = av_frame_clone(avframe);
        if (ref) {
          packet->set_ai_frame(ref);
        } else {
          Error("LPR: cannot reference frame for annotation");
        }
      }
    }
  }

  return recognised;
}

#endif  // HAVE_QUADRA
