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

#ifndef ZM_NETINT_LPR_H
#define ZM_NETINT_LPR_H

#include <string>
#include <vector>

/* The postprocess maths below is deliberately outside the HAVE_QUADRA guard: it
 * depends on nothing from libxcoder, so it stays compilable — and unit
 * testable — on builds without a Quadra card present.
 */
namespace zm_lpr {

// Character set used to turn recogniser class indices into text. Index 0 is the
// CTC blank. The built-in set is the one the bundled reclicense model was
// trained on (CCPD: Chinese province glyphs, then digits and A-Z less I/O).
const std::vector<std::string> &default_charset();

/* Load a charset from a file, one UTF-8 entry per line, index 0 being the
 * blank. Returns the built-in set if the file cannot be read, so a recogniser
 * retrained for another region can be dropped in beside its .nb without a code
 * change. Blank lines are preserved as entries, since a charset legitimately
 * may not use '#' to spell its blank.
 */
std::vector<std::string> load_charset(const std::string &path);

/* Split a UTF-8 string into one string per codepoint. */
std::vector<std::string> utf8_codepoints(const std::string &text);

/* Greedy CTC decode of the recogniser output.
 *
 * tensor is [max_chars][chars_num] row-major: for each of the max_chars
 * timesteps, chars_num class scores. Takes the argmax per timestep, then
 * collapses runs of the same class and drops the blank (index 0).
 *
 * NOTE: this differs deliberately from NetInt's ni_lpr sample, which tracks the
 * last *emitted* character rather than the last raw one. That makes the sample
 * swallow a genuinely doubled character that the model separated with a blank
 * ("A blank A" decodes to "A", not "AA") — exactly the case the CTC blank
 * exists to disambiguate. Doubled characters are common in non-Chinese plates,
 * so we do the standard collapse-then-strip instead.
 */
std::string ctc_decode(const float *tensor, int chars_num, int max_chars,
                       const std::vector<std::string> &charset);

/* Order four detected corner landmarks into top-left, top-right, bottom-left,
 * bottom-right — the order the perspective warp expects. Corners are picked by
 * extremes of (x+y) and (y-x), so the ordering holds for a rotated plate.
 */
void order_landmarks(const double in[4][2], double out[4][2]);

/* Compute the 9 perspective coefficients mapping the destination rect
 * (out_w x out_h) back onto the source quad given by landmark[4][2], which must
 * be in the order produced by order_landmarks().
 *
 * The resulting f[] is used as:
 *   u = 256 * (f[0]*x + f[1]*y + f[2]) / (f[6]*x + f[7]*y + f[8])
 *   v = 256 * (f[3]*x + f[4]*y + f[5]) / (f[6]*x + f[7]*y + f[8])
 * giving source coordinates in 1/256th-pixel fixed point for destination pixel
 * (x, y).
 */
void perspective_coeffs(const double landmark[4][2], int out_w, int out_h, double f[9]);

/* Reduce a decoded plate to something ZoneMinder's font can actually draw.
 *
 * Image::Annotate walks the string a byte at a time and indexes a fixed bitmap
 * font with each byte, so a multi-byte codepoint is drawn as several blank
 * glyphs - the label silently loses characters and its spacing drifts. Replace
 * each non-ASCII codepoint with a single '?' so what is drawn stays aligned with
 * what was read. The full text still goes to the detection JSON and the log.
 */
std::string ascii_label(const std::string &utf8);

}  // namespace zm_lpr

#if HAVE_QUADRA

#include "zm_avfilter_worker.h"
#include "zm_ffmpeg.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include "netint_network.h"
#include "yolo_postprocess.h"

#include <memory>
#include <nlohmann/json.hpp>

class Monitor;
class ZMPacket;

/* One detected plate: its box in source-image coordinates plus the four corner
 * landmarks the detector regressed for it.
 */
struct PlateBox {
  roi_box box;
  double landmark[4][2];
};

// A plate that made it through both stages, ready to be drawn.
struct RecognisedPlate {
  roi_box box;
  std::string text;
};

/* Two-stage licence plate recognition on a NetInt Quadra card.
 *
 * Stage 1 is a YOLOv5-shaped detector whose layers carry 8 extra regression
 * channels encoding 4 corner landmarks per plate. Stage 2 takes each plate,
 * deskews it through a perspective warp built from those landmarks, and runs a
 * recogniser whose output is CTC-decoded into text.
 *
 * This runs chained after Quadra_Yolo on the same frame, and holds its own two
 * network contexts on the card.
 */
class Quadra_LPR {
 public:
  Quadra_LPR(Monitor *p_monitor, bool p_use_hwframe);
  ~Quadra_LPR();
  Quadra_LPR(const Quadra_LPR &) = delete;
  Quadra_LPR &operator=(const Quadra_LPR &) = delete;

  bool setup(AVStream *p_dec_stream, AVCodecContext *p_dec_ctx,
             const std::string &det_nbg_file, const std::string &rec_nbg_file,
             int deviceid);

  /* Run both stages over the packet's frame. Appends one entry per recognised
   * plate to packet->detections. Returns the number of plates recognised,
   * 0 if none, or negative on an error that should tear the session down.
   */
  int detect(const std::shared_ptr<ZMPacket> &packet);

 private:
  int run_detector(AVFrame *avframe, std::vector<PlateBox> &plates);
  int recognise_plate(AVFrame *avframe, const PlateBox &plate, std::string &text);
  int generate_det_frame(ni_session_data_io_t *ai_frame, AVFrame *avframe);
  int get_plate_boxes(int img_width, int img_height, std::vector<PlateBox> &plates);
  void sample_perspective(const AVFrame *in, AVFrame *out, const double landmark[4][2]);

  /* Draw a box round each plate and label it with the number, in place on the
   * frame - the same approach Quadra_Yolo takes for its detections.
   */
  void annotate(AVFrame *frame, const std::vector<RecognisedPlate> &plates);
  bool create_det_model(ni_network_data_t *network_data);
  void destroy_det_model();

  /* Returns a software frame to work on, downloading from the card if needed.
   * The returned frame is owned by the caller only when *owned is set.
   */
  AVFrame *software_frame(const std::shared_ptr<ZMPacket> &packet, bool *owned);

  Monitor *monitor;
  AVStream *dec_stream;
  AVCodecContext *dec_ctx;

  // Stage 1: detector
  NiNetworkContext *det_network;
  YoloModelCtx det_model;
  NiNetworkFrame det_frame;
  SwsContext *det_scale_ctx;
  int det_width;
  int det_height;

  // Stage 2: recogniser
  NiNetworkContext *rec_network;
  NiNetworkFrame rec_frame;
  SwsContext *rec_scale_ctx;
  int rec_width;
  int rec_height;
  int chars_num;   // recogniser output classes per timestep
  int max_chars;   // recogniser output timesteps
  std::vector<float> rec_tensor;
  std::vector<float> rec_planar;
  av_frame_ptr persp_frame;  // deskewed plate, YUV420P
  av_frame_ptr rgb_frame;    // persp_frame converted to planar GBR

  std::vector<std::string> charset;

  float obj_thresh;
  float nms_thresh;

  bool use_hwframe;
  bool models_created;
  bool draw_annotations;

  filter_worker hwdl_filter;
};

#endif  // HAVE_QUADRA
#endif  // ZM_NETINT_LPR_H
