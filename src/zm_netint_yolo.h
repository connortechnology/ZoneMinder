#ifndef ZM_QUADRA_YOLO_H
#define ZM_QUADRA_YOLO_H

#include "zm_signal.h"
#include "zm_ffmpeg.h"
#include "zm_avfilter_worker.h"
#include "zm_object_classes.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include "yolo_model.h"
#include "netint_network.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#define NI_TRANSCODE_FRAME_NUM 3
#define NI_SAME_CENTER_THRESH 2
#define NI_SAME_BORDER_THRESH 8

class Monitor;
class ZMPacket;

/* Which frame inference should run against. A free function so the choice can
 * be unit tested on its own: it touches nothing from libxcoder.
 */
namespace zm_yolo {
// Returns the frame to feed the AI session, or nullptr when this packet cannot
// be inferred on and should be skipped.
//
// A hardware session was allocated for device input -- use_hwframe is fixed at
// construction and baked into ni_alloc_network_context -- so only a frame
// carrying a device surface in data[3] will serve it. There is deliberately no
// falling back to the software frame: that reaches ni_hwframe_scale with a null
// surface and dereferences it. A null return is expected under load rather than
// a fault, because the decoder hands hw_frame back to the card once we are at
// the device frame budget.
AVFrame *ai_input_frame(bool use_hwframe, AVFrame *hw_frame, AVFrame *in_frame);
}  // namespace zm_yolo

class Quadra_Yolo {
  private:
    Monitor *monitor;
    int model_width;
    int model_height;
    int model_format;
    bool model_bgr;  // true if model expects BGR channel order, false for RGB
    ObjectClasses object_classes_;  // Class labels (defaults to COCO, can load from .names file)
    float obj_thresh = 0.25;
    float nms_thresh = 0.45;
    NiNetworkContext *network_ctx;
    YoloModel *model;
    YoloModelCtx *model_ctx;
    NiNetworkFrame net_frame;
    //ni_session_data_io_t *ai_frame;

    av_frame_ptr scaled_frame;
    //SWScale swscale;
    SwsContext *sw_scale_ctx;

    // Letterbox parameters for aspect ratio preservation
    int letterbox_offset_x = 0;  // X offset of scaled image within model frame
    int letterbox_offset_y = 0;  // Y offset of scaled image within model frame
    int letterbox_width = 0;     // Width of scaled image (without padding)
    int letterbox_height = 0;    // Height of scaled image (without padding)
    float letterbox_scale = 1.0f; // Scale factor applied to original image

    filter_worker hwdl_filter;

    bool drawbox;
    filter_worker drawbox_filter;

    bool drawtext;
    filter_worker drawtext_filter;
    // Needed for format conversion
    filter_worker scale_to_rgba_filter;
    filter_worker scale_to_yuv420p_filter;

    AVStream *dec_stream;
    AVCodecContext *dec_ctx;

    int aiframe_number;
    AVRegionOfInterest *last_roi;
    AVRegionOfInterestNetintExtra *last_roi_extra;
    int last_roi_count;

    // Annotation cost, split by the two operations, so the software and
    // hardware paths can be compared on the same monitor. Accumulated per
    // detection and reported as a mean, because a per-detection line at
    // frame rate is unreadable and a single sample says nothing.
    uint64_t annotate_box_us_ = 0;
    uint64_t annotate_text_us_ = 0;
    // The mean hid a bimodal cost: most drawtext calls are a few ms, a
    // minority block for seconds. Keep the shape, not just the average.
    uint64_t drawtext_calls_ = 0;
    uint64_t drawtext_slow_calls_ = 0;
    uint64_t drawtext_max_us_ = 0;
    // Records one drawtext call, reporting it individually when it blocks.
    void record_drawtext_time(uint64_t us, size_t labels);
    uint64_t annotate_count_ = 0;
    // Highest drawtext slot written last frame, so a quieter frame can blank
    // what a busier one left set.
    size_t drawtext_slots_used_ = 0;

    // av_opt_set failing is silent otherwise: the filter simply draws nothing
    // and the log shows a successful call that cost the price of a few string
    // formats. Count them, and report the first in full.
    uint64_t drawtext_opt_errors_ = 0;
    bool drawtext_opt_reported_ = false;
    bool drawbox_opt_reported_ = false;
    bool drawbox_cmd_reported_ = false;
    bool drawbox_width_reported_ = false;
    // NI_MAX_SUPPORT_DRAWBOX_NUM: the rectangles ni_quadra_drawbox carries.
    static constexpr int kDrawboxSlots = 5;
    // MAX_TEXT_NUM: the text slots ni_quadra_drawtext carries.
    static constexpr size_t kDrawtextSlots = 32;
    // As with drawtext, a narrower border must clear what a wider one set.
    int drawbox_slots_used_ = 0;
    int set_drawbox_opt(int slot, const char *name, int value);

    bool use_hwframe;
    nlohmann::json detections;

    int filt_cnt;
  public:
    Quadra_Yolo(Monitor *p_monitor, bool p_use_hwframe);
    ~Quadra_Yolo();
    bool setup(AVStream *p_dec_stream, AVCodecContext *decoder_ctx, const std::string &model_name="", const std::string &nbg_file="", int deviceid=-1);
    bool setup_drawbox();
    bool setup_drawtext();
    int send_packet(std::shared_ptr<ZMPacket> in_packet);
    int receive_detection(std::shared_ptr<ZMPacket> out_packet);
    int detect(std::shared_ptr<ZMPacket>in_packet, std::shared_ptr<ZMPacket> out_packet);
    int draw_last_roi(std::shared_ptr<ZMPacket> packet);
    int draw_text(AVFrame *input, AVFrame **output, const std::string &text, int x, int y, const std::string &colour);
  private:
    int annotate(AVFrame *input, AVFrame **output, const AVRegionOfInterest &roi, const AVRegionOfInterestNetintExtra &roi_extra, Rgb box_color = 0);
    int draw_roi_box(AVFrame *inframe, AVFrame **outframe, AVRegionOfInterest roi, AVRegionOfInterestNetintExtra roi_extra, int line_width, Rgb box_color = 0);
    int draw_roi_box_in_place(AVFrame *inframe, AVRegionOfInterest roi, AVRegionOfInterestNetintExtra roi_extra, int line_width, Rgb box_color = 0);
    int ni_recreate_ai_frame(ni_frame_t *ni_frame, AVFrame *frame);
    int generate_ai_frame(ni_session_data_io_t *ai_frame, AVFrame *avframe, bool hwframe);
    int process_roi(AVFrame *frame, AVFrame **filt_frame);
    int check_movement( AVRegionOfInterest cur_roi, AVRegionOfInterestNetintExtra cur_roi_extra);
    // One label to draw. Collected for a whole frame and drawn in a single
    // pass: ni_quadra_drawtext takes up to 32 texts at once (t0-t31 and
    // friends), and every separate call costs a filter reinit, which re-runs
    // init() and reloads the font through fontconfig.
    struct TextItem {
      std::string text;
      int x = 0;
      int y = 0;
      std::string colour;
    };
    int draw_texts(AVFrame *in_frame, AVFrame **output, const std::vector<TextItem> &items);

    int ni_read_roi(AVFrame *out, int frame_count);
    bool parse_model_file(const std::string &nbg_file);
};

#endif
