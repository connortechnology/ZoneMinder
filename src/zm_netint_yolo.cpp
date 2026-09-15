#include "config.h"

#if HAVE_QUADRA

#include "zm_logger.h"
#include "zm_ffmpeg.h"
#include "zm_monitor.h"
#include "zm_object_classes.h"
#include "zm_vector2.h"

#include "zm_netint_yolo.h"

#include <algorithm>
#include <cstring>
#include <fstream>

// NetInt .nb model file format offsets
constexpr size_t NB_MODEL_NAME_OFFSET = 0x0C;
constexpr size_t NB_MODEL_NAME_MAX_LEN = 64;

#define SOFTWARE_DRAWBOX 0

// Background drawn behind a label. Dark enough to carry white text over a
// bright scene, transparent enough to keep what is behind it visible.
static constexpr const char *kLabelBackground = "black@0.5";

// A call over this is not a slow blit, it is a wait: the filter allocates its
// output from the frame pool and blocks when the card has none to give.
static constexpr uint64_t kDrawtextBlockedUs = 250000;


namespace zm_yolo {

AVFrame *ai_input_frame(bool use_hwframe, AVFrame *hw_frame, AVFrame *in_frame) {
  if (!use_hwframe) return in_frame;
  if (hw_frame and hw_frame->data[3]) return hw_frame;
  return nullptr;
}

}  // namespace zm_yolo

Quadra_Yolo::Quadra_Yolo(Monitor *p_monitor, bool p_use_hwframe) :
  monitor(p_monitor),
  model_width(640),
  model_height(640),
  model_format(GC620_RGB888_PLANAR),
  model_bgr(true),  // Default to BGR as most NetInt models use BGR
  obj_thresh(0.25),
  nms_thresh(0.45),
  network_ctx(nullptr),
  model(nullptr),
  model_ctx(nullptr),
  net_frame(),
  sw_scale_ctx(nullptr),

  //hwdl_filter(nullptr),
  drawbox(true),
  //drawbox_filter(nullptr),
  //drawbox_filter_ctx(nullptr),
  drawtext(true),
  //drawtext_filter(nullptr),
  //drawtext_filter_ctx(nullptr),

  dec_stream(nullptr),
  dec_ctx(nullptr),

  aiframe_number(0),
  last_roi(nullptr),
  last_roi_extra(nullptr),
  last_roi_count(0),

  use_hwframe(p_use_hwframe),
  filt_cnt(0)
{
  obj_thresh = monitor->ObjectDetection_Object_Threshold();
  nms_thresh = monitor->ObjectDetection_NMS_Threshold();
}

Quadra_Yolo::~Quadra_Yolo() {
  ni_frame_buffer_free(&net_frame.api_frame.data.frame);
  ni_packet_buffer_free(&net_frame.api_packet.data.packet);
  if (network_ctx)
    ni_cleanup_network_context(network_ctx, use_hwframe);
  if (model && model_ctx) {
    model->destroy_model(model_ctx);
    delete model_ctx;
    model_ctx = nullptr;
  }

  free(last_roi);
  free(last_roi_extra);

  if (sw_scale_ctx) {
    sws_freeContext(sw_scale_ctx);
    sw_scale_ctx = nullptr;
  }
}

bool Quadra_Yolo::parse_model_file(const std::string &nbg_file) {
  std::ifstream file(nbg_file, std::ios::binary);
  if (!file.is_open()) {
    Warning("Cannot open model file %s to parse header", nbg_file.c_str());
    return false;
  }

  // Verify magic number "VPMN"
  char magic[4];
  file.read(magic, 4);
  if (!file || std::strncmp(magic, "VPMN", 4) != 0) {
    Warning("Model file %s does not have VPMN magic header", nbg_file.c_str());
    return false;
  }

  // Read model name string at offset 0x0C (contains dimensions and color info)
  char model_name_buf[NB_MODEL_NAME_MAX_LEN + 1] = {0};
  file.seekg(NB_MODEL_NAME_OFFSET);
  file.read(model_name_buf, NB_MODEL_NAME_MAX_LEN);
  if (!file) {
    Warning("Cannot read model name from %s", nbg_file.c_str());
    return false;
  }
  std::string model_name_str(model_name_buf);
  Debug(1, "Model name from file header: %s", model_name_str.c_str());

  // Detect color order from model name
  if (model_name_str.find("bgr") != std::string::npos ||
      model_name_str.find("BGR") != std::string::npos) {
    model_bgr = true;
    Debug(1, "Detected BGR color order from model header");
  } else if (model_name_str.find("rgb") != std::string::npos ||
             model_name_str.find("RGB") != std::string::npos) {
    model_bgr = false;
    Debug(1, "Detected RGB color order from model header");
  }

  // Dimensions are read from network_data after ni_ai_config_network_binary
  // loads the model in setup(), so we don't parse them from the file header.

  return true;
}

bool Quadra_Yolo::setup(
    AVStream *p_dec_stream,
    AVCodecContext *decoder_ctx,
    const std::string &modelname,
    const std::string &nbg_file,
    int deviceid)
{
  std::string model_name = modelname;
  dec_stream = p_dec_stream;
  dec_ctx = decoder_ctx;

  // Detect model type from filename
  if (std::string::npos != nbg_file.find("yolov4")) {
    model_name = std::string("yolov4");
  } else if (std::string::npos != nbg_file.find("yolov5")) {
    model_name = std::string("yolov5");
  } else if (std::string::npos != nbg_file.find("yolov8")) {
    model_name = std::string("yolov8");
  }

  // Parse model file header for color order
  parse_model_file(nbg_file);

  // Load class names from .names file (falls back to COCO if not found)
  object_classes_.loadFromFile(nbg_file);

  // Set model interface based on detected type
  if (model_name == "yolov4") {
    model = &yolov4;
  } else if (model_name == "yolov5") {
    model = &yolov5;
  } else if (model_name == "yolov8") {
    model = &yolov8;
  } else {
    Error("Unsupported yolo model");
    return false;
  }

  Debug(1, "Model %s: color order: %s",
      nbg_file.c_str(), model_bgr ? "BGR" : "RGB");

  //std::string device = monitor->DecoderHWAccelDevice();
  //int devid = device.empty() ? -1 : std::stoi(device);
  int devid = deviceid;

  // Pass 0,0 for dimensions initially - let ni_ai_config_network_binary determine
  // the actual model dimensions, then we'll read them from network_data
  Debug(1, "Setup NETint %s using %s on %d, use hwframe %d", model_name.c_str(), nbg_file.c_str(), devid, use_hwframe);
  int ret = ni_alloc_network_context(&network_ctx, use_hwframe,
      devid /*dev_id*/, 30 /* keep alive */, model_format, 0, 0, nbg_file.c_str());
  if (ret != 0) {
    Error("failed to allocate network context on card %d", devid);
    model = nullptr;
    return false;
  }

  // Read actual model dimensions from the loaded network data
  model_width = static_cast<int>(network_ctx->network_data.linfo.in_param[0].sizes[0]);
  model_height = static_cast<int>(network_ctx->network_data.linfo.in_param[0].sizes[1]);
  Debug(1, "Actual model dimensions from network_data: %dx%d", model_width, model_height);

  model_ctx = new YoloModelCtx();
  ret = model->create_model(model_ctx, &network_ctx->network_data, obj_thresh, nms_thresh, model_width, model_height);
  if (ret != 0) {
    Error("failed to initialize yolo model");
    return false;
  }

  net_frame.scale_width = model_width;
  net_frame.scale_height = model_height;
  net_frame.scale_format = model_format;

  ret = ni_ai_packet_buffer_alloc(&net_frame.api_packet.data.packet, &network_ctx->network_data);
  if (ret != NI_RETCODE_SUCCESS) {
    Error( "failed to allocate packet on card %d", devid);
    return false;
  }
  ret = ni_frame_buffer_alloc_hwenc(&net_frame.api_frame.data.frame,
      net_frame.scale_width,
      net_frame.scale_height, 0);
  if (ret != NI_RETCODE_SUCCESS) {
    Error("failed to allocate frame on card %d", devid);
    return false;
  }

  if (use_hwframe == false) {
    scaled_frame = av_frame_ptr{av_frame_alloc()};
    scaled_frame->width  = model_width;
    scaled_frame->height = model_height;
    scaled_frame->format = AV_PIX_FMT_RGB24;

    if (av_frame_get_buffer(scaled_frame.get(), 0)) {
      Error("cannot allocate scaled frame buffer");
      return false;
    }
  }

#if !SOFTWARE_DRAWBOX
  if (drawbox) drawbox = drawbox_filter.setup("ni_quadra_drawbox=inplace=1", "ni_quadra_drawbox", dec_ctx, dec_stream->time_base, dec_ctx->hw_frames_ctx, dec_ctx->pix_fmt);
  //monitor->LabelSize() // 1234
  if (drawtext) {
    const int fontsize = 10 + monitor->LabelSize() * 4;
    // White on a bright scene is unreadable, which is why the software
    // blitter drew onto black. box fills the glyph box before the text, and
    // ff_blend_rectangle honours the alpha, so a part-transparent black still
    // shows the scene through it.
    //
    // box is one flag for the filter, but the colour and the padding are per
    // slot, so every slot we might use has to be set. They are set here
    // rather than per frame because they never change, and because they are
    // the options NETINT did not mark runtime -- a reinit command carrying
    // them is refused. reinit runs av_opt_copy from the live context before
    // applying its own string, so what is set here survives every reinit.
    // The filter takes an output frame from its own pool for every frame it
    // draws, and that pool defaults to four. Four in flight is not enough
    // while this monitor also holds decoded frames of its own:
    // ni_scaler_session_read_hwdesc gets frame index 0 and spins for seconds,
    // with ni_rsrc_mon showing the scaler at 0% load and 0% frame memory.
    // The card has the frames; the session's pool does not.
    //
    // Set as an option rather than through filter_ctx->extra_hw_frames, which
    // is libavfilter's own accounting and fixed once the filter is
    // initialised: a pool bigger than that field leaves these frames outside
    // the hardware frames context hwdownload was configured for, and it
    // refuses every one with EINVAL. Needs
    // utils/netint/ffmpeg-patches/0001, which adds the option.
    const int budget = monitor->DeviceFrameBudget() > 0
        ? monitor->DeviceFrameBudget()
        : static_cast<int>(zm_device_frame_budget());
    const int pool_size = std::max(8, budget / 2);

    std::string options = stringtf(
        "ni_quadra_drawtext=text=init:expansion=none:fontsize=%d:font=Sans:box=1"
        ":pool_size=%d",
        fontsize, pool_size);
    Debug(1, "drawtext output pool of %d frames (budget %d)", pool_size, budget);
    const int padding = std::max(2, fontsize / 6);
    for (size_t i = 0; i < kDrawtextSlots; i++)
      options += stringtf(":bc%zu=%s:bb%zu=%d", i, kLabelBackground, i, padding);

    drawtext = drawtext_filter.setup(options, "ni_quadra_drawtext",
        dec_ctx, dec_stream->time_base, dec_ctx->hw_frames_ctx, dec_ctx->pix_fmt);
  }
  //if (drawtext) drawtext = drawtext_filter.setup("ni_quadra_scale=iw:ih:format=rgba,ni_quadra_drawtext=text=init:fontsize=24:font=Sans", "ni_quadra_drawtext", true, dec_ctx->pix_fmt);
#endif

  return true;
}

/* Ideally we could handle intermixed hwframes and swframes. */
int Quadra_Yolo::send_packet(std::shared_ptr<ZMPacket> in_packet) {
  AVFrame *avframe = zm_yolo::ai_input_frame(use_hwframe, in_packet->hw_frame.get(), in_packet->in_frame.get());
  if (!avframe) {
    if (use_hwframe) {
      // Expected under load: at the device frame budget the decoder gives the
      // frame back to the card and carries on from the software copy, which a
      // hardware AI session cannot read. Skip inference for this packet.
      Debug(1, "No device frame for packet %d; skipping inference", in_packet->image_index);
    } else {
      Error("No frame in packet %d to run inference on", in_packet->image_index);
    }
    return 0;
  }

  if (!use_hwframe && !sw_scale_ctx) {
    // Calculate letterbox parameters to preserve aspect ratio
    float scale_x = static_cast<float>(model_width) / avframe->width;
    float scale_y = static_cast<float>(model_height) / avframe->height;
    letterbox_scale = std::min(scale_x, scale_y);

    letterbox_width = static_cast<int>(avframe->width * letterbox_scale);
    letterbox_height = static_cast<int>(avframe->height * letterbox_scale);

    // Center the scaled image within the model frame
    letterbox_offset_x = (model_width - letterbox_width) / 2;
    letterbox_offset_y = (model_height - letterbox_height) / 2;

    Debug(1, "Letterbox: source %dx%d -> scaled %dx%d, offset (%d,%d), scale %.3f",
        avframe->width, avframe->height, letterbox_width, letterbox_height,
        letterbox_offset_x, letterbox_offset_y, letterbox_scale);

    // Scale to letterbox size (not full model size)
    sw_scale_ctx = sws_getContext(
        avframe->width, avframe->height, static_cast<AVPixelFormat>(avframe->format),
        letterbox_width, letterbox_height, static_cast<AVPixelFormat>(scaled_frame->format),
        SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sw_scale_ctx) {
      Error("cannot create sw scale context for scaling hwframe: %d", use_hwframe);
      return -1;
    }
  }

  ni_session_data_io_t ai_input_frame = {};

	zm_dump_video_frame(avframe, "Quadra: generate_ai_frame");
  int ret = generate_ai_frame(&ai_input_frame, avframe, use_hwframe);
  if (ret < 0) {
    Error("Quadra: cannot generate ai frame");
    return -1;
  }

  ret = ni_set_network_input(network_ctx, use_hwframe, &ai_input_frame, nullptr, avframe->width, avframe->height, &net_frame, true);
  if (ret != 0 && ret != NIERROR(EAGAIN)) {
    Error("Quadra: Error while feeding the ai");
    return -1;
  }
  return (ret == NIERROR(EAGAIN)) ? 0 : 1;
} // int Quadra_Yolo::send_packet(std::shared_ptr<ZMPacket> in_packet)

/* NetInt says if an image goes in, one will come out, so we can assume that 
 * out_packet corresponds to the image that the results are against.
 */
int Quadra_Yolo::receive_detection(std::shared_ptr<ZMPacket> packet) {
  SystemTimePoint starttime = std::chrono::system_clock::now();
  /* pull filtered frames from the filtergraph */
  int ret = ni_get_network_output(network_ctx, use_hwframe, &net_frame, true /* blockable */, true /*convert*/, model_ctx->out_tensor);
  SystemTimePoint endtime = std::chrono::system_clock::now();
  Debug(1, "Quadra: *** AI inference took %.2f seconds ***",FPSeconds(endtime-starttime).count());
  if (ret != 0 && ret != NIERROR(EAGAIN)) {
    // A non-EAGAIN error from ni_get_network_output usually means the AI session
    // was closed by the firmware keep-alive timeout (30s) because inference fell
    // too far behind real-time to touch the session in time. The device output
    // shows "wrong session ID, got 0xffff" / "keep alive thread has been closed".
    // Recovery requires reducing load: lower the capture frame rate, run AI on
    // fewer frames, or reduce the number of concurrent AI streams on the card.
    Error("Quadra: Error when getting AI output %d. "
          "The AI session was likely closed after inference fell behind real-time — "
          "reduce capture FPS, decimate AI frames, or reduce card load.",
          ret);
    return -1;
  } else if (ret != NIERROR(EAGAIN)) {
    AVFrame *avframe = (use_hwframe and packet->hw_frame) ? packet->hw_frame.get() : packet->in_frame.get();
    Debug(1, "Quadra: receive_detection hw_frame %p, data %p ai_frame_number %d", avframe, avframe->data, aiframe_number);
    ret = ni_read_roi(avframe, aiframe_number);
    if (ret < 0) {
      Error("Quadra: read roi failed");
      return -1;
    } else if (ret == 0) {
      Debug(1, "Quadra: ni_read_roi == 0");
      return 1;
    }
    aiframe_number++;
    AVFrame *out_frame = nullptr;
    // Allocates out_frame
    ret = process_roi(avframe, &out_frame);
    if (ret < 0) {
      Error("Quadra: cannot draw roi");
      return -1;
    }
    if (!out_frame) {
      Error("Quadra: process_roi returned %d without a frame", ret);
      return -1;
    }
    packet->set_ai_frame(out_frame);
    zm_dump_video_frame(out_frame, "ai");
  } else {
    return 0;
  }

  packet->detections = detections;
  return 1;
} // end receive_detection

int Quadra_Yolo::ni_recreate_ai_frame(ni_frame_t *ni_frame, AVFrame *avframe) {
  uint8_t *p_data = ni_frame->p_data[0];

  Debug(3,
      "linesize %d/%d/%d, data %p/%p/%p, pixel %dx%d",
      avframe->linesize[0], avframe->linesize[1], avframe->linesize[2],
      avframe->data[0], avframe->data[1], avframe->data[2], avframe->width,
      avframe->height);

  if (avframe->format == AV_PIX_FMT_RGB24) {
    /* RGB24 -> planar format (BGR or RGB depending on model) with letterboxing */
    const int model_plane_size = model_width * model_height;
    uint8_t *first_data  = p_data;
    uint8_t *second_data = p_data + model_plane_size;
    uint8_t *third_data  = p_data + model_plane_size * 2;
    uint8_t *fdata  = avframe->data[0];

    // Use letterbox dimensions if set, otherwise fall back to model dimensions (no letterboxing)
    int copy_width = (letterbox_width > 0) ? letterbox_width : model_width;
    int copy_height = (letterbox_height > 0) ? letterbox_height : model_height;
    int offset_x = (letterbox_width > 0) ? letterbox_offset_x : 0;
    int offset_y = (letterbox_height > 0) ? letterbox_offset_y : 0;

    Debug(1, "%s(): rgb24 to %s planar with letterbox, scaled %dx%d -> model %dx%d, offset (%d,%d)",
        __func__, model_bgr ? "bgr" : "rgb", copy_width, copy_height,
        model_width, model_height, offset_x, offset_y);

    // Fill entire buffer with black (padding)
    std::memset(first_data, 0, model_plane_size);
    std::memset(second_data, 0, model_plane_size);
    std::memset(third_data, 0, model_plane_size);

    // Copy scaled image into center position with letterbox offset
    // Use copy dimensions which account for letterbox or fallback to model size
    for (int y = 0; y < copy_height; y++) {
      for (int x = 0; x < copy_width; x++) {
        int fpos = y * avframe->linesize[0];
        // Position in model buffer includes letterbox offset
        int out_x = x + offset_x;
        int out_y = y + offset_y;
        int ppos = out_y * model_width + out_x;

        uint8_t r = fdata[fpos + x * 3 + 0];
        uint8_t g = fdata[fpos + x * 3 + 1];
        uint8_t b = fdata[fpos + x * 3 + 2];

        if (model_bgr) {
          // BGR planar: B first, G second, R third
          first_data[ppos] = b;
          second_data[ppos] = g;
          third_data[ppos] = r;
        } else {
          // RGB planar: R first, G second, B third
          first_data[ppos] = r;
          second_data[ppos] = g;
          third_data[ppos] = b;
        }
      }
    }
  } else {
    Error("cannot recreate frame: invalid frame format");
  }
  return 0;
}

int Quadra_Yolo::generate_ai_frame(ni_session_data_io_t *ai_frame, AVFrame *avframe, bool hwframe) {
  int ret = 0;

  if (!hwframe) {
    ret = sws_scale(sw_scale_ctx, (const uint8_t * const *)avframe->data,
        avframe->linesize, 0, avframe->height, scaled_frame->data, scaled_frame->linesize);
    if (ret < 0) {
      Error("cannot do sw scale: inframe data 0x%lx, linesize %d/%d/%d/%d, height %d to %d linesize",
          (unsigned long)avframe->data, avframe->linesize[0], avframe->linesize[1],
          avframe->linesize[2], avframe->linesize[3], avframe->height, scaled_frame->linesize[0]);
      return ret;
    }
    AVFrame *test = scaled_frame.get();
    zm_dump_video_frame(test, "Quadra: scale_frame");
    ni_retcode_t retval = ni_ai_frame_buffer_alloc(&ai_frame->data.frame, &network_ctx->network_data);
    if (retval != NI_RETCODE_SUCCESS) {
      Error("cannot allocate sw ai frame buffer");
      return NIERROR(ENOMEM);
    }
    ret = ni_recreate_ai_frame(&ai_frame->data.frame, scaled_frame.get());
    if (ret < 0) {
      Error("cannot recreate sw ai frame");
      return ret;
    }
  } else {
    ai_frame->data.frame.p_data[3] = avframe->data[3];
  }

  return ret;
}

int Quadra_Yolo::draw_roi_box_in_place(
    AVFrame *inframe,
    AVRegionOfInterest roi,
    AVRegionOfInterestNetintExtra roi_extra,
    int line_width,
    Rgb box_color) {

  // Use provided color, or fall back to default class-based color
  if (box_color == 0) {
    box_color = object_classes_.boxColorFor(roi_extra.cls);
  }
  Image in_image(inframe);

  for (int i=0; i<line_width; i++) {
    in_image.DrawBox(roi.left+i, roi.top+i, roi.right-i, roi.bottom-i, box_color);
  }
  return 1;
} // end draw_roi_box_in_place
 
int Quadra_Yolo::draw_roi_box(
    AVFrame *inframe,
    AVFrame **outframe,
    AVRegionOfInterest roi,
    AVRegionOfInterestNetintExtra roi_extra,
    int line_width,
    Rgb box_color) {

  SystemTimePoint starttime = std::chrono::system_clock::now();

  // Convert Rgb to color string for filter, or use default class-based color
  const char *color;
  char color_buf[16];
  if (box_color != 0) {
    snprintf(color_buf, sizeof(color_buf), "0x%06X", box_color);
    color = color_buf;
  } else {
    color = object_classes_.colorStringFor(roi_extra.cls);
  }

  // The filter carries kDrawboxSlots rectangles and draws each as a one pixel
  // outline, so a thick border is that many nested rectangles drawn in one
  // pass. Writing them all to the unsuffixed x/y/w/h left only the last
  // iteration set, which is why the borders came out a single pixel wide,
  // inset by line_width-1.
  int slots = line_width;
  if (slots > kDrawboxSlots) {
    if (!drawbox_width_reported_) {
      drawbox_width_reported_ = true;
      Warning("line width %d exceeds the %d boxes the filter carries; "
              "drawing a %d pixel border", line_width, kDrawboxSlots, kDrawboxSlots);
    }
    slots = kDrawboxSlots;
  }

  int r = 0;
  for (int i = 0; i < slots; i++) {
    int x = roi.left + i;
    int y = roi.top + i;
    int w = (roi.right - roi.left) - 2*i;
    int h = (roi.bottom - roi.top) - 2*i;

    // A rectangle that has closed up contributes nothing, and a negative one
    // is refused outright by the filter.
    if (w <= 0 or h <= 0) {
      slots = i;
      break;
    }

    Debug(1, "draw_roi_box: slot %d x %d, y %d, w %d, h %d color %s line_width %d",
        i, x, y, w, h, color, line_width);
    // Checked for the same reason as the drawtext slots: a refused option
    // leaves the filter drawing the previous box, or none, and says nothing.
    if (set_drawbox_opt(i, "x", x) < 0) r = -1;
    if (set_drawbox_opt(i, "y", y) < 0) r = -1;
    if (set_drawbox_opt(i, "w", w) < 0) r = -1;
    if (set_drawbox_opt(i, "h", h) < 0) r = -1;
  }

  // A wider border left slots set that this one does not use, and the filter
  // draws every slot whose width and height are positive. Zero the tail.
  for (int i = slots; i < drawbox_slots_used_; i++) {
    set_drawbox_opt(i, "w", 0);
    set_drawbox_opt(i, "h", 0);
  }
  drawbox_slots_used_ = slots;

  if (r < 0 and !drawbox_opt_reported_) {
    drawbox_opt_reported_ = true;
    Error("drawbox rejected a geometry option; boxes will not follow detections");
  }
  int cmd_ret = drawbox_filter.send_command("ni_quadra_drawbox", "color", color);
  if (cmd_ret < 0 and !drawbox_cmd_reported_) {
    drawbox_cmd_reported_ = true;
    Error("drawbox rejected the colour command: %d %s; boxes will use the previous colour",
          cmd_ret, av_make_error_string(cmd_ret).c_str());
  }

  int ret = drawbox_filter.execute(inframe, outframe);
  SystemTimePoint endtime = std::chrono::system_clock::now();
  Debug(1, "draw_roi_box took: %.2f seconds %d", FPSeconds(endtime - starttime).count(), ret);
  return ret;
} // end draw_roi_box

/* frame is the output from the model, not an image. Just raw info in the side data
 * file_frame is where we will stick the image with the bounding boxes.
 */
int Quadra_Yolo::process_roi(AVFrame *in_frame, AVFrame **filt_frame) {

  AVFrameSideData *sd = in_frame->side_data ? av_frame_get_side_data(in_frame, AV_FRAME_DATA_REGIONS_OF_INTEREST) : nullptr;
  AVFrameSideData *sd_roi_extra = in_frame->side_data ? av_frame_get_side_data( in_frame, AV_FRAME_DATA_NETINT_REGIONS_OF_INTEREST_EXTRA) : nullptr;

  Debug(4, "Filt %d frame pts %3" PRId64, ++filt_cnt, in_frame->pts);
	zm_dump_video_frame(in_frame, "Quadra: process_roi in_frame");
  if (!sd || !sd_roi_extra || sd->size == 0 || sd_roi_extra->size == 0) {
    // A reference, not the caller's own pointer. This returns into
    // packet->set_ai_frame(), which takes ownership, and in_frame is
    // packet->hw_frame or packet->in_frame -- already owned by that same
    // packet. Handing it straight back left the packet owning one AVFrame
    // twice, and ~ZMPacket freed it twice: SIGABRT out of _int_free, on
    // every frame the model found nothing in.
    *filt_frame = av_frame_clone(in_frame);
    if (*filt_frame == nullptr) {
      Error("cannot clone frame");
      return NIERROR(ENOMEM);
    }
    Debug(1, "no roi area in frame %d", filt_cnt);
    return 0;
  }

  AVFrame *input = nullptr;
#if SOFTWARE_DRAWBOX
  if (in_frame->hw_frames_ctx) {
#else
  if (!use_hwframe && in_frame->hw_frames_ctx) {
#endif
    if (!hwdl_filter.initialised) {
      if (!hwdl_filter.setup("hwdownload,format=yuv420p", "", dec_ctx, dec_stream->time_base, in_frame->hw_frames_ctx, dec_ctx->pix_fmt)) {
        Warning("No hwdl");
        return -1;
      }
    }

    // Allocates the frame, gets the image from hw
    Debug(1, "**** hwdl start ****");
    int ret = hwdl_filter.execute(in_frame, &input);
    Debug(1, "**** hwdl stop ****");
    if (ret < 0) {
      Error("cannot download hwframe");
      return ret;
    }
    zm_dump_video_frame(input, "Quadra: process_roi input");
  } else {
    input = in_frame;
  }

  AVRegionOfInterest *roi = (AVRegionOfInterest *)sd->data;
  AVRegionOfInterestNetintExtra *roi_extra = (AVRegionOfInterestNetintExtra *)sd_roi_extra->data;
  if ((sd->size % roi->self_size) ||
      (sd_roi_extra->size % roi_extra->self_size) ||
      (sd->size / roi->self_size !=
       sd_roi_extra->size / roi_extra->self_size)) {
    Error( "invalid roi side data");
    return NIERROR(EINVAL);
  }
  int num = sd->size / roi->self_size;

  detections = nlohmann::json::array();
  std::vector<TextItem> text_items;
  Debug(1, "Num, detections %d from sd %ld size / roi size %d", num, sd->size, roi->self_size);

  for (int i = 0; i < num; i++) {
    std::string class_name = object_classes_.getClassName(roi_extra[i].cls);
    std::array<int, 4> bbox = {roi[i].left, roi[i].top, roi[i].right, roi[i].bottom};
    nlohmann::json detection = {{"class", class_name}, {"bbox", bbox}, {"score", roi_extra[i].prob}};

    // Filter through monitor's AI detection settings
    nlohmann::json temp_array = nlohmann::json::array();
    temp_array.push_back(detection);
    nlohmann::json filtered = monitor->FilterDetections(temp_array);
    if (filtered.empty()) {
      Debug(3, "Quadra: Filtering out detection of class '%s'", class_name.c_str());
      continue;  // Skip this detection - filtered out
    }

    // Detection passed filtering, add to results and annotate
    nlohmann::json filtered_detection = filtered[0];
    detections.push_back(filtered_detection);

    // Get box color from filtered detection (set by FilterDetections)
    Rgb box_color = 0;
    if (filtered_detection.contains("box_color") && !filtered_detection["box_color"].is_null()) {
      box_color = filtered_detection["box_color"].get<Rgb>();
    }

    AVFrame *output = nullptr;
    annotate(input, &output, roi[i], roi_extra[i], box_color);
    if (output) {
      if (input != in_frame and input != output) av_frame_free(&input);
      input = output;
    }
    if (drawtext) {
      text_items.push_back({
          stringtf("%s %.1f%%", class_name.c_str(), 100*roi_extra[i].prob),
          roi[i].left + 1 + monitor->LabelSize(),
          roi[i].top + 1 + monitor->LabelSize(),
          "white"});
    }
  } // end foreach roi

  // Every label on this frame in one pass. See draw_texts().
  if (!text_items.empty()) {
    SystemTimePoint text_starttime = std::chrono::system_clock::now();
    AVFrame *text_output = nullptr;
    int ret = draw_texts(input, &text_output, text_items);
    if (ret < 0) {
      Error("cannot draw labels %d %s", ret, av_make_error_string(ret).c_str());
    } else if (text_output) {
      if (input != in_frame) av_frame_free(&input);
      input = text_output;
    }
    record_drawtext_time(std::chrono::duration_cast<Microseconds>(
        std::chrono::system_clock::now() - text_starttime).count(),
        text_items.size());
  }
  
  AVFrame *output = nullptr;
  // Allocates the frame, gets the image from hw
#if SOFTWARE_DRAWBOX
  if (0) {
#else
  if (use_hwframe) {
#endif
    if (hwdl_filter.initialised or hwdl_filter.setup("hwdownload,format=yuv420p", "", dec_ctx, dec_stream->time_base, input->hw_frames_ctx, dec_ctx->pix_fmt)) {
      zm_dump_video_frame(input, "Quadra: process_roi hwframe");
      Debug(1, "*** Start of hwdownload ***");
      int ret = hwdl_filter.execute(input, &output);
      Debug(1, "*** End   of hwdownload ***");
      if (ret < 0) {
        Error("cannot download hwframe");
        output = input;
      } else {
        zm_dump_video_frame(output, "Quadra: process_roi output");
      }
    } else {
      output = input;
    }
  } else {
    output = input;
  }

  *filt_frame = output;
  if (input != in_frame and input != output) av_frame_free(&input);
  
  // Technicall we should have a lock around this, but since we only access from the Analysis thread, we won't worry about it.
  // // Not sure why we free/allocate, would be more efficient to allocate the max and just overwrite.
  free(last_roi);
  free(last_roi_extra);
  AVRegionOfInterest* cur_roi = nullptr;
  AVRegionOfInterestNetintExtra *cur_roi_extra = nullptr;

  if (num > 0) {
    cur_roi = (AVRegionOfInterest *)av_malloc((int)(num * sizeof(AVRegionOfInterest)));
    cur_roi_extra = (AVRegionOfInterestNetintExtra *)av_malloc((int)(num * sizeof(AVRegionOfInterestNetintExtra)));
    for (int i = 0; i < num; i++) {
      (cur_roi[i]).self_size = roi[i].self_size;
      (cur_roi[i]).top       = roi[i].top;
      (cur_roi[i]).bottom    = roi[i].bottom;
      (cur_roi[i]).left      = roi[i].left;
      (cur_roi[i]).right     = roi[i].right;
      (cur_roi[i]).qoffset   = { 0 , 1 };  // 0/1: no QP adjustment. den=0 is rejected by the encoder.
      cur_roi_extra[i].self_size = roi_extra[i].self_size;
      cur_roi_extra[i].cls       = roi_extra[i].cls;
      cur_roi_extra[i].prob      = roi_extra[i].prob;
    }
  }
  last_roi = cur_roi;
  last_roi_extra = cur_roi_extra;
  last_roi_count = num;

  // We don't need the size data any more.  So free it.  Probably not worth it.
  av_frame_side_data_free(&in_frame->side_data, &in_frame->nb_side_data);

  return detections.size();
}

int Quadra_Yolo::annotate(
    AVFrame *in_frame, AVFrame **output,
    const AVRegionOfInterest &roi,
    const AVRegionOfInterestNetintExtra &roi_extra,
    Rgb box_color
    ) {
  AVFrame *input = in_frame;
  if (drawbox) {
    SystemTimePoint box_starttime = std::chrono::system_clock::now();
#if SOFTWARE_DRAWBOX
    int ret = draw_roi_box_in_place(input, roi, roi_extra, monitor->LabelSize(), box_color);
    if (ret < 0) Error("draw roi box failed");
#else
    AVFrame *drawbox_output = nullptr;
    zm_dump_video_frame(input, "Quadra: drawbox input");
    int ret = draw_roi_box(input, &drawbox_output, roi, roi_extra, monitor->LabelSize(), box_color);
    if (ret < 0) {
      Error("draw roi box failed %d %s", ret, av_make_error_string(ret).c_str());
    } else {
      // These are all pointers... we don't want to free in_frame (yet) theoretically, but we do want to free the intermediate outputs.
      // So, this SHOULD leave in_frame alone..., free input, assign output to input
      Debug(1, "input %p =? in_frame %p drawbox_output %p", input, in_frame, drawbox_output);
      if ((input != in_frame) && (input != drawbox_output)) {
        av_frame_free(&input);
      } else if (drawbox_output) {
        input = drawbox_output;
      }
      zm_dump_video_frame(input, "Quadra: drawbox output");
    }
#endif
    annotate_box_us_ += std::chrono::duration_cast<Microseconds>(
        std::chrono::system_clock::now() - box_starttime).count();
  }  // end if drawbox


  // Report the means rather than each detection: at frame rate with several
  // detections a frame, a line per call is unreadable, and one sample of a
  // filter says nothing about its cost.
  annotate_count_++;
  if (annotate_count_ % 100 == 0) {
    Debug(1, "Annotation over %ju detections (%s): drawbox %.2fms each, "
             "drawtext %ju calls mean %.2fms max %.2fms, %ju blocked over %.0fms"
             ", %ju reinits%s",
        static_cast<uintmax_t>(annotate_count_),
        SOFTWARE_DRAWBOX ? "software" : "hardware filters",
        annotate_box_us_ / 1000.0 / annotate_count_,
        static_cast<uintmax_t>(drawtext_calls_),
        drawtext_calls_ ? annotate_text_us_ / 1000.0 / drawtext_calls_ : 0.0,
        drawtext_max_us_ / 1000.0,
        static_cast<uintmax_t>(drawtext_slow_calls_),
        kDrawtextBlockedUs / 1000.0,
        static_cast<uintmax_t>(drawtext_reinits_),
        // A timing means nothing if the options were refused, so never print
        // one that looks respectable without saying the labels are missing.
        drawtext_opt_errors_
            ? stringtf(" -- %ju option rejections, labels NOT drawn",
                       static_cast<uintmax_t>(drawtext_opt_errors_)).c_str()
            : "");
  }
  *output = input;
  // So in_frame should not be touched, and we should have an output frame, that references the same data as in_frame.
  return 1;
} // end annotate

int Quadra_Yolo::draw_last_roi(std::shared_ptr<ZMPacket> packet) {
  if (!last_roi_count) return 1;

#if SOFTWARE_DRAWBOX
  if (packet->needs_hw_transfer(dec_ctx)) {
    if (!packet->transfer_hwframe(dec_ctx)) {
      return -1;
    }
  }; // Just in case it hasn't been done yet
  AVFrame *in_frame = av_frame_alloc();
  av_frame_ref(in_frame, packet->in_frame.get());
#else
  AVFrame *in_frame = packet->hw_frame.get();
#endif
  AVFrame *input = in_frame;
  std::vector<TextItem> text_items;

  if (!input) return 1;

  for (int i = 0; i < last_roi_count; i++) {
    // Apply same filtering as in process_roi
    std::string class_name = object_classes_.getClassName(last_roi_extra[i].cls);
    nlohmann::json detection = {{"class", class_name}, {"score", last_roi_extra[i].prob}};
    nlohmann::json temp_array = nlohmann::json::array();
    temp_array.push_back(detection);
    nlohmann::json filtered = monitor->FilterDetections(temp_array);
    if (filtered.empty()) {
      Debug(3, "Quadra: draw_last_roi filtering out detection of class '%s'", class_name.c_str());
      continue;  // Skip this detection - filtered out
    }

    // Get box color from filtered detection (set by FilterDetections)
    nlohmann::json filtered_detection = filtered[0];
    Rgb box_color = 0;
    if (filtered_detection.contains("box_color") && !filtered_detection["box_color"].is_null()) {
      box_color = filtered_detection["box_color"].get<Rgb>();
    }

    AVFrame *output = nullptr;
    // Annotate doesn't touch input, should return output, which is a frame pointing to the same data as input.
    annotate(input, &output, last_roi[i], last_roi_extra[i], box_color);
    if (output) {
      // This is incorrect, and is only here as a test.
      if (input != in_frame) av_frame_free(&input);
      input = output;
    }
    if (drawtext) {
      text_items.push_back({
          stringtf("%s %.1f%%", class_name.c_str(), 100*last_roi_extra[i].prob),
          last_roi[i].left + 1 + monitor->LabelSize(),
          last_roi[i].top + 1 + monitor->LabelSize(),
          "white"});
    }
  } // end foreach detection

  // Every label in one pass, as in process_roi.
  if (!text_items.empty()) {
    SystemTimePoint text_starttime = std::chrono::system_clock::now();
    AVFrame *text_output = nullptr;
    int ret = draw_texts(input, &text_output, text_items);
    if (ret < 0) {
      Error("cannot draw labels %d %s", ret, av_make_error_string(ret).c_str());
    } else if (text_output) {
      if (input != in_frame) av_frame_free(&input);
      input = text_output;
    }
    record_drawtext_time(std::chrono::duration_cast<Microseconds>(
        std::chrono::system_clock::now() - text_starttime).count(),
        text_items.size());
  }

#if !SOFTWARE_DRAWBOX
  if (!hwdl_filter.initialised) {
    if (!hwdl_filter.setup("hwdownload,format=yuv420p", "", dec_ctx, dec_stream->time_base, input->hw_frames_ctx, dec_ctx->pix_fmt)) {
      Warning("No hwdl");
      return -1;
    }
  }

  AVFrame *output = nullptr;
  // Allocates the frame, gets the image from hw
  Debug(1, "*** hwdownload start ***");
  int ret = hwdl_filter.execute(input, &output);
  Debug(1, "*** hwdownload stop ***");
  if (ret < 0) {
    Error("cannot download hwframe");
  }
  if (input != in_frame) av_frame_free(&input);
  input = output;
#endif
  if (input) {
    zm_dump_video_frame(input, "Quadra:draw_last_roi input");
    packet->ai_frame = std::move(av_frame_ptr{input}); // Should really be ai_frame
  } else {
    Error("No output from get_gwdl");
  }
  return 1;
} // end int Quadra_Yolo::draw_last_roi(std::shared_ptr<ZMPacket> packet)

int Quadra_Yolo::check_movement(
    AVRegionOfInterest cur_roi,
    AVRegionOfInterestNetintExtra cur_roi_extra)
{
    for (int i = 0; i < last_roi_count; i++) {
        if (last_roi_extra[i].cls == cur_roi_extra.cls) {
            if (abs(cur_roi.left - last_roi[i].left) < NI_SAME_BORDER_THRESH &&
                abs(cur_roi.right - last_roi[i].right) < NI_SAME_BORDER_THRESH &&
                abs(cur_roi.top - last_roi[i].top) < NI_SAME_BORDER_THRESH &&
                abs(cur_roi.bottom - last_roi[i].bottom) < NI_SAME_BORDER_THRESH &&
                abs((last_roi[i].left + last_roi[i].right) / 2 - (cur_roi.left + cur_roi.right) / 2) < NI_SAME_CENTER_THRESH &&
                abs((last_roi[i].top + last_roi[i].bottom) / 2 - (cur_roi.top + cur_roi.bottom) / 2) < NI_SAME_CENTER_THRESH) {
                    return 1;
                }
        }
    }
    return 0;
}

int Quadra_Yolo::ni_read_roi(AVFrame *out, int frame_count) {
  struct roi_box *roi_box = nullptr;
  int roi_num = 0;

  // For software letterboxing, pass model dimensions to get model-space coordinates.
  // The letterbox transformation will then correctly map them to original image space.
  // For hwframe mode (no letterboxing), pass original frame dimensions for direct scaling.
  int box_width = (!use_hwframe && letterbox_scale > 0.0f) ? model_width : out->width;
  int box_height = (!use_hwframe && letterbox_scale > 0.0f) ? model_height : out->height;

  Debug(2, "ni_get_boxes: using %dx%d for coordinate space (letterbox=%s, hwframe=%s)",
      box_width, box_height,
      (letterbox_scale > 0.0f) ? "yes" : "no",
      use_hwframe ? "yes" : "no");

  int ret = model->ni_get_boxes(model_ctx, box_width, box_height, &roi_box, &roi_num);
  if (ret < 0) {
    Error("failed to get roi.");
    if (roi_box) free(roi_box);
    return ret;
  }

  if (roi_num == 0) {
    Debug(1, "no roi available");
    if (roi_box) free(roi_box);
    return 0;
  }
#if 0
  if (0) {
    pr_err("frame %d roi num %d\n", frame_count, roi_num);
    for (i = 0; i < roi_num; i++) {
      pr_err("frame count %d roi %d: top %d, bottom %d, left %d, right %d, class %d name %s prob %f\n",
          frame_count, i, roi_box[i].top, roi_box[i].bottom, roi_box[i].left,
          roi_box[i].right, roi_box[i].class, object_classes_.getClassName(roi_box[i].class).c_str(), roi_box[i].prob);
    }
  }
  for (i = 0; i < roi_num; i++) {
    if (roi_box[i].ai_class == 0 || roi_box[i].ai_class == 7) {
      reserve_roi_num++;
    }
  }
  if (reserve_roi_num == 0) {
    Debug(1, "no reserve roi available");
    ret = 0;
    goto out;
  }
#endif
  AVFrameSideData *sd = av_frame_new_side_data(out, AV_FRAME_DATA_REGIONS_OF_INTEREST, (int)(roi_num * sizeof(AVRegionOfInterest)));
  AVFrameSideData *sd_roi_extra = av_frame_new_side_data( out, AV_FRAME_DATA_NETINT_REGIONS_OF_INTEREST_EXTRA, (int)(roi_num * sizeof(AVRegionOfInterestNetintExtra)));
  if (!sd || !sd_roi_extra) {
    ret = NIERROR(ENOMEM);
    Error("Failed to allocate roi sidedata %ld roi_num %d ret:%d", roi_num * sizeof(AVRegionOfInterestNetintExtra), roi_num, ret);
    if (roi_box) free(roi_box);
    return ret;
  }

  AVRegionOfInterest * roi = (AVRegionOfInterest *)sd->data;
  AVRegionOfInterestNetintExtra *roi_extra = (AVRegionOfInterestNetintExtra *)sd_roi_extra->data;
  int i, j;

  // Coordinate transformation from model space to original image space:
  // 1. ni_get_boxes returns coordinates in model space (0 to model_width/height)
  // 2. With letterboxing, the actual image occupies only part of the model input
  //    (centered with black padding)
  // 3. We need to: subtract the letterbox offset, then divide by the scale factor
  // Formula: original_coord = (model_coord - letterbox_offset) / letterbox_scale
  auto transform_coord = [this, out](int coord, int offset, bool is_vertical) -> int {
    if (letterbox_scale <= 0.0f || use_hwframe) {
      return coord;  // No letterbox transformation needed (hwframe or no letterbox)
    }
    // Convert from model space to original image space
    int transformed = static_cast<int>((coord - offset) / letterbox_scale);
    int max_val = is_vertical ? out->height - 1 : out->width - 1;
    return std::clamp(transformed, 0, max_val);
  };

  for (i = 0, j = 0; i < roi_num; i++) {
    roi[j].self_size = sizeof(*roi);
    // Transform coordinates from model space to original image space
    roi[j].left      = transform_coord(roi_box[i].left, letterbox_offset_x, false);
    roi[j].right     = transform_coord(roi_box[i].right, letterbox_offset_x, false);
    roi[j].top       = transform_coord(roi_box[i].top, letterbox_offset_y, true);
    roi[j].bottom    = transform_coord(roi_box[i].bottom, letterbox_offset_y, true);
    roi[j].qoffset   = { 0 , 1 };  // 0/1: no QP adjustment. den=0 is rejected by the encoder.
    roi_extra[j].self_size = sizeof(*roi_extra);
    roi_extra[j].cls       = roi_box[i].ai_class;
    roi_extra[j].prob      = roi_box[i].prob;
    Debug(3, "roi %d: model(%d,%d,%d,%d) -> image(%d,%d,%d,%d) [offset(%d,%d) scale=%.3f], class %d prob %.2f",
        j, roi_box[i].left, roi_box[i].top, roi_box[i].right, roi_box[i].bottom,
        roi[j].left, roi[j].top, roi[j].right, roi[j].bottom,
        letterbox_offset_x, letterbox_offset_y, letterbox_scale,
        roi_extra[j].cls, roi_extra[j].prob);
    j++;
  }
  if (roi_box) free(roi_box);
  return roi_num;
}

void Quadra_Yolo::record_drawtext_time(uint64_t us, size_t labels) {
  drawtext_calls_++;
  annotate_text_us_ += us;
  if (us > drawtext_max_us_) drawtext_max_us_ = us;
  if (us > kDrawtextBlockedUs) {
    drawtext_slow_calls_++;
    Warning("drawtext blocked %.2fs on %zu label%s (%ju of %ju calls so far)",
        us / 1000000.0, labels, labels == 1 ? "" : "s",
        static_cast<uintmax_t>(drawtext_slow_calls_),
        static_cast<uintmax_t>(drawtext_calls_));
  }
}

// The reinit command carries an option string parsed on ':' and '=', so a
// value containing either has to be escaped or it splits the command.
static std::string escape_filter_value(const std::string &value) {
  std::string out;
  for (char c : value) {
    if (c == '\\' or c == ':' or c == '=' or c == '\'') out += '\\';
    out += c;
  }
  return out;
}

// Slot 0 is the unsuffixed x/y/w/h, the rest carry their index.
int Quadra_Yolo::set_drawbox_opt(int slot, const char *name, int value) {
  std::string key = slot ? stringtf("%s%d", name, slot) : name;
  return drawbox_filter.opt_set(key, value);
}


int Quadra_Yolo::draw_texts(AVFrame *in_frame, AVFrame **output,
                            const std::vector<TextItem> &items) {
  if (items.empty()) {
    *output = nullptr;
    return 1;
  }

#if SOFTWARE_DRAWBOX
  Image img(in_frame);
  for (const TextItem &item : items) {
    // Black rather than transparent, for the same reason the hardware path
    // draws a box: white on a bright scene cannot be read. Annotate has no
    // alpha, so this is the opaque version of what the filter does.
    img.Annotate(item.text.c_str(), Vector2(item.x, item.y),
                 monitor->LabelSize(), kRGBWhite, kRGBBlack);
  }
  *output = nullptr;   // drawn in place, caller keeps in_frame
  return 1;
#else
  if (!drawtext_filter.filter_ctx) {
    Error("drawtext filter not configured");
    return -1;
  }

  // One command for every label on the frame. The filter carries 32 slots, so
  // anything beyond that is dropped rather than silently overwriting slot 31.
  size_t count = items.size();
  if (count > kDrawtextSlots) {
    Warning("%zu labels on one frame, drawing the first %zu", count, kDrawtextSlots);
    count = kDrawtextSlots;
  }

  // Set every slot through the filter's own reinit command rather than
  // av_opt_set on filter_ctx->priv. opt_set reaches the option storage but
  // not what init() derives from it, and this filter derives two things that
  // matter: text_num is counted once in init() by walking text[] to the first
  // null, so with a single text at graph construction only slot 0 is ever
  // drawn however many we set; and x/y are evaluated from the pre-parsed
  // x_pexpr/y_pexpr, not from the strings, so a new position is ignored.
  // That is why the boxes moved with the detections and the labels did not
  // appear at all.
  //
  // reinit re-runs uninit() and init(), which recomputes both. It used to
  // fail with EINVAL because NETINT never gave these options
  // AV_OPT_FLAG_RUNTIME_PARAM, which FFmpeg 7.x requires on an initialised
  // object; utils/netint/ffmpeg-patches/0002 adds it.
  std::string command;
  for (size_t i = 0; i < count; i++) {
    if (!command.empty()) command += ":";
    command += stringtf("t%zu=%s:x%zu=%d:y%zu=%d:fc%zu=%s",
        i, escape_filter_value(items[i].text).c_str(),
        i, items[i].x,
        i, items[i].y,
        i, items[i].colour.c_str());
  }

  // Slots a busier frame left behind would otherwise redraw its labels over
  // this one. A slot must stay non-empty for text_num to count past it, so
  // blank it with a space rather than dropping it.
  for (size_t i = count; i < drawtext_slots_used_; i++) {
    command += stringtf(":t%zu=%s", i, " ");
  }
  drawtext_slots_used_ = count;

  // update sets the options on the live filter and re-derives text_num, the
  // position expressions and any newly used slot's glyphs. reinit was doing
  // the same job by building a fresh context and uninit()ing the old one,
  // which closed both Quadra scaler sessions and rebuilt both frame pools
  // every time a label changed. At 6 to 12 changes a second that starved the
  // card: drawtext went back to blocking for seconds in
  // ni_scaler_session_read_hwdesc and the decoder stopped making its frame
  // budget, so frames were dropped and went out with no box and no label.
  // utils/netint/ffmpeg-patches/0003 adds the command.
  //
  // Still only sent when something changed: draw_last_roi redraws the last
  // detection on every frame between inferences, so most commands would set
  // what is already set.
  if (command == drawtext_last_command_) {
    Debug(2, "Drawtext unchanged, %zu label%s", count, count == 1 ? "" : "s");
    return drawtext_filter.execute(in_frame, output);
  }

  Debug(1, "Drawtext %zu label%s: %s", count, count == 1 ? "" : "s", command.c_str());
  drawtext_reinits_++;
  int ret = drawtext_filter.send_command("ni_quadra_drawtext", "update", command.c_str());
  if (ret < 0) {
    drawtext_opt_errors_++;
    if (!drawtext_opt_reported_) {
      drawtext_opt_reported_ = true;
      Error("drawtext refused update: %d %s. Labels will not be drawn; the "
            "command comes from utils/netint/ffmpeg-patches/0003 and needs "
            "0002's AV_OPT_FLAG_RUNTIME_PARAM. Command was: %s",
            ret, av_make_error_string(ret).c_str(), command.c_str());
    }
    return ret;
  }
  drawtext_last_command_ = command;

  return drawtext_filter.execute(in_frame, output);
#endif
}

int Quadra_Yolo::draw_text(AVFrame *input, AVFrame **output, const std::string &text, int x, int y, const std::string &colour) {
  if (!drawtext_filter.filter_ctx) {
    Error("drawtext filter not configured");
    return -1;
  }

  Debug(1, "Drawtext: %s %dx%d %s", text.c_str(), x, y, colour.c_str());
#if 0
  drawtext_filter.opt_set("x", x);
  drawtext_filter.opt_set("y", y);
  drawtext_filter.opt_set("text", text.c_str());
  drawtext_filter.opt_set("fontcolor", colour.c_str());
#else
  std::string drawtext_option = stringtf("text='%s':x=%d:y=%d:fontcolor=%s", text.c_str(), x, y, colour.c_str());
  int ret = avfilter_graph_send_command(drawtext_filter.filter_graph, "ni_quadra_drawtext", "reinit", drawtext_option.c_str(), NULL, 0, 0);
  if (ret < 0) {
    Error("cannot send drawtext filter command %d %s: %s", ret, av_make_error_string(ret).c_str(), drawtext_option.c_str());
    return ret;
  }
#endif

  int rc;
  Debug(1, "*** Start of drawtext ***");
  rc = drawtext_filter.execute(input, output);
  Debug(1, "*** End   of drawtext ***");
  return rc;
}

#endif
