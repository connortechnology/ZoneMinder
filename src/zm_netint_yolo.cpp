#include "config.h"

#if HAVE_QUADRA

#include "zm_logger.h"
#include "zm_ffmpeg.h"
#include "zm_monitor.h"
#include "zm_vector2.h"

#include "zm_netint_yolo.h"

static const char *roi_class[] = {"person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat",
  "traffic light", "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat",
  "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
  "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball",
  "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket",
  "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
  "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair",
  "couch", "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote",
  "keyboard", "cell phone", "microwave", "oven", "toaster", "sink", "refrigerator", "book",
  "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"};

AVRational qp_offset = { 0 , 0 };

Quadra_Yolo::Quadra_Yolo(Monitor *p_monitor, bool p_use_hwframe) :
  monitor(p_monitor),
  model_width(640),
  model_height(640),
  model_format(GC620_RGB888_PLANAR),
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
  scaled_frame = av_frame_ptr{zm_av_frame_alloc()};
  scaled_frame->width  = model_width;
  scaled_frame->height = model_height;
  scaled_frame->format = AV_PIX_FMT_RGB24;
  obj_thresh = monitor->ObjectDetection_Object_Threshold();
  nms_thresh = monitor->ObjectDetection_NMS_Threshold();
}

Quadra_Yolo::~Quadra_Yolo() {
  ni_frame_buffer_free(&net_frame.api_frame.data.frame);
  ni_packet_buffer_free(&net_frame.api_packet.data.packet);
  if (network_ctx)
    ni_cleanup_network_context(network_ctx, use_hwframe);
  if (model) {
    model->destroy_model(model_ctx);
    delete model_ctx;
  }

  free(last_roi);
  free(last_roi_extra);

  if (sw_scale_ctx) {
    sws_freeContext(sw_scale_ctx);
    sw_scale_ctx = nullptr;
  }
}

bool Quadra_Yolo::setup(
    AVStream *p_dec_stream,
    AVCodecContext *decoder_ctx,
    const std::string &modelname,
    const std::string &nbg_file,
    int deviceid)
{
  dec_stream = p_dec_stream;
  dec_ctx = decoder_ctx;
  model_ctx = new YoloModelCtx;
  if (model_ctx == nullptr) {
    Error("failed to allocate yolo model");
    return false;
  }

  //std::string device = monitor->DecoderHWAccelDevice();
  //int devid = device.empty() ? -1 : std::stoi(device);
  int devid = deviceid;

  Debug(1, "Setup NETint %s on %d, use hwframe %d", modelname.c_str(), devid, use_hwframe);
  int ret = ni_alloc_network_context(&network_ctx, use_hwframe,
      devid /*dev_id*/, 30 /* keep alive */, model_format, model_width, model_height, nbg_file.c_str());
  if (ret != 0) {
    Error("failed to allocate network context on card %d", devid);
    return false;
  }

  if (modelname == "yolov4") {
      model = &yolov4;
  } else if (modelname == "yolov5") {
      model = &yolov5;
      model_width = model_height = 640;
  } else if (modelname == "yolov8") {
      model = &yolov8;
      model_width = model_height = 640;
  } else {
      Error("Unsupported yolo model");
      return false;
  }

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
    scaled_frame->width  = model_width;
    scaled_frame->height = model_height;
    scaled_frame->format = AV_PIX_FMT_RGB24;

    if (av_frame_get_buffer(scaled_frame.get(), 0)) {
      Error("cannot allocate scaled frame buffer");
      return false;
    }
  }

  if (drawbox) drawbox = drawbox_filter.setup(this, "ni_quadra_scale=iw:ih:format=rgba,ni_quadra_drawbox,ni_quadra_scale=iw:ih:format=yuv420p", "ni_quadra_drawbox", dec_ctx->hw_frames_ctx, dec_ctx->pix_fmt);
  //monitor->LabelSize() // 1234
  if (drawtext) drawtext = drawtext_filter.setup(this, stringtf("ni_quadra_drawtext=text=init:fontsize=%d:font=Sans",10+monitor->LabelSize() * 4), "ni_quadra_drawtext", dec_ctx->hw_frames_ctx, dec_ctx->pix_fmt);
  //if (drawtext) drawtext = drawtext_filter.setup(this, "ni_quadra_scale=iw:ih:format=rgba,ni_quadra_drawtext=text=init:fontsize=24:font=Sans", "ni_quadra_drawtext", true, dec_ctx->pix_fmt);


  return true;
}

/* Ideally we could hangle intermixed hwframes and swframes. */
int Quadra_Yolo::send_packet(std::shared_ptr<ZMPacket> in_packet) {
  AVFrame *avframe = (use_hwframe and in_packet->hw_frame) ? in_packet->hw_frame.get() : in_packet->in_frame.get();
  if (!in_packet->hw_frame) {
    Error("No hw_frame in packet!");
  }

  if (!use_hwframe && !sw_scale_ctx) {
    sw_scale_ctx = sws_getContext(
        avframe->width, avframe->height, static_cast<AVPixelFormat>(avframe->format),
        scaled_frame->width, scaled_frame->height, static_cast<AVPixelFormat>(scaled_frame->format),
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
    Error("Error while feeding the ai");
    return -1;
  }
  return (ret == NIERROR(EAGAIN)) ? 0 : 1;
} // int Quadra_Yolo::send_packet(std::shared_ptr<ZMPacket> in_packet)

/* in_packet and out_packet maybe be th same*/
int Quadra_Yolo::detect(std::shared_ptr<ZMPacket> in_packet, std::shared_ptr<ZMPacket> out_packet) {
  int ret = send_packet(in_packet);
  if (ret <= 0) return ret;

  return ret = receive_detection(out_packet);
} // end int Quadra_Yolo::detect(std::shared_ptr<ZMPacket> in_packet, std::shared_ptr<ZMPacket> out_packet)

/* NetInt says if an image goes in, one will come out, so we can assume that 
 * out_packet corresponds to the image that the results are against.
 */
int Quadra_Yolo::receive_detection(std::shared_ptr<ZMPacket> out_packet) {
  /* pull filtered frames from the filtergraph */
  int ret = ni_get_network_output(network_ctx, use_hwframe, &net_frame, true /* blockable */, true /*convert*/, model_ctx->out_tensor);
  if (ret != 0 && ret != NIERROR(EAGAIN)) {
    Error("Error when getting output %d", ret);
    return -1;
  } else if (ret != NIERROR(EAGAIN)) {
    AVFrame *hw_frame = out_packet->hw_frame.get();
    Debug(1, "hw_frame %p, data %p ai_frame_number %d", hw_frame, hw_frame->data, aiframe_number);
    ret = ni_read_roi(hw_frame, aiframe_number);
    if (ret < 0) {
      Error("read roi failed");
      return -1;
    } else if (ret == 0) {
      Debug(1, "ni_read_roi == 0");
      return 1;
    }
    aiframe_number++;
    AVFrame *out_frame;
    // Allocates out_frame
    ret = process_roi(hw_frame, &out_frame);
    if (ret < 0) {
      Error("cannot draw roi");
      return -1;
    }
    out_packet->set_ai_frame(out_frame);
    zm_dump_video_frame(out_frame, "ai");
  } else {
    return 0;
  }

  out_packet->detections = std::move(detections);
  return 1;
} // end receive_detection

int Quadra_Yolo::ni_recreate_ai_frame(ni_frame_t *ni_frame, AVFrame *avframe) {
  uint8_t *p_data = ni_frame->p_data[0];

  Debug(1,
      "linesize %d/%d/%d, data %p/%p/%p, pixel %dx%d",
      avframe->linesize[0], avframe->linesize[1], avframe->linesize[2],
      avframe->data[0], avframe->data[1], avframe->data[2], avframe->width,
      avframe->height);

  if (avframe->format == AV_PIX_FMT_RGB24) {
    /* RGB24 -> BGRP */
    uint8_t *r_data = p_data + avframe->width * avframe->height * 2;
    uint8_t *g_data = p_data + avframe->width * avframe->height * 1;
    uint8_t *b_data = p_data + avframe->width * avframe->height * 0;
    uint8_t *fdata  = avframe->data[0];
    int x, y;

    Debug(1, "%s(): rgb24 to bgrp, pix %dx%d, linesize %d", __func__,
        avframe->width, avframe->height, avframe->linesize[0]);

    for (y = 0; y < avframe->height; y++) {
      for (x = 0; x < avframe->width; x++) {
        int fpos  = y * avframe->linesize[0];
        int ppos  = y * avframe->width;
        uint8_t r = fdata[fpos + x * 3 + 0];
        uint8_t g = fdata[fpos + x * 3 + 1];
        uint8_t b = fdata[fpos + x * 3 + 2];

        r_data[ppos + x] = r;
        g_data[ppos + x] = g;
        b_data[ppos + x] = b;
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

int Quadra_Yolo::draw_roi_box(
    AVFrame *inframe,
    AVFrame **outframe,
    AVRegionOfInterest roi,
    AVRegionOfInterestNetintExtra roi_extra,
    int line_width=1) {

  int cls = roi_extra.cls;
  std::string color;
  if (cls == 0) {
    color = "Blue";
  } else {
    color = "Red";
  }
  while(line_width > 0) {
    line_width--;
  int x = roi.left + line_width; // line
  int y = roi.top + line_width;
  int w = (roi.right - roi.left) - 2*line_width;
  int h = (roi.bottom - roi.top) - 2*line_width;

  Debug(4, "x %d, y %d, w %d, h %d color %s", x, y, w, h, color.c_str());

#if 1
  drawbox_filter.opt_set(stringtf("x%d", line_width), x);
  drawbox_filter.opt_set(stringtf("y%d", line_width), y);
  drawbox_filter.opt_set(stringtf("w%d", line_width), w);
  drawbox_filter.opt_set(stringtf("h%d", line_width), h);
  drawbox_filter.opt_set(stringtf("color%d", line_width), color.c_str());
#else
  std::string option = stringtf("x=%d:y=%d:w=%d:h=%d:color=%s", x, y, w, h, color.c_str());
  int ret = avfilter_graph_send_command(drawbox_filter.filter_graph, "ni_quadra_drawbox", "reinit", option.c_str(), NULL, 0, 0);

  //int ret = avfilter_graph_send_command(drawbox_filter.filter_graph, "ni_quadra_drawbox", "color", color.c_str(), nullptr, 0, 0);
  if (ret < 0) {
    Error("cannot send drawbox filter command, ret %d.", ret);
    //return ret;
  }
#endif
  }

  return drawbox_filter.execute(inframe, outframe);
} // end draw_roi_box

int Quadra_Yolo::init_filter(const char *filters_desc, filter_worker *f, AVBufferRef *hw_frames_ctx, AVPixelFormat input_fmt) {
  char args[512] = { 0 };
  char name[32] = { 0 };
  int i, ret = 0;
  AVFilterInOut *inputs, *outputs, *cur;

  f->filter_graph = avfilter_graph_alloc();
  if (!f->filter_graph) {
    Error("failed to allocate filter graph");
    return AVERROR(ENOMEM);
  }

  ret = avfilter_graph_parse2(f->filter_graph, filters_desc, &inputs, &outputs);
  if (ret < 0) {
    Error("failed to parse graph for %s", filters_desc);
    return ret;
  }

  // link input
  cur = inputs, i = 0;
  AVRational time_base = dec_stream->time_base;

  snprintf(name, sizeof(name), "in_%d", i);
  snprintf(args, sizeof(args),
      "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d",
      dec_ctx->width, dec_ctx->height, input_fmt, time_base.num, time_base.den,
      dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den,
      dec_ctx->framerate.num,dec_ctx->framerate.den);
  Debug(1, "Setting filter %s to %s", name, args);

  ret = avfilter_graph_create_filter(&f->buffersrc_ctx, avfilter_get_by_name("buffer"), name, args, nullptr, f->filter_graph);
  if (ret < 0) {
    Error("Cannot create buffer source");
    return ret;
  }

  // decoder out=hw
  if (hw_frames_ctx) {
    Debug(1, "hw mode filter");
    // Allocate a new AVBufferSrcParameters instance when decoder out=hw
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    if (!par) {
      Error("cannot allocate hwdl buffersrc parameters");
      return ret = AVERROR(ENOMEM);
    }
    memset(par, 0, sizeof(*par));
    // set format and hw_frames_ctx to AVBufferSrcParameters when out=hw
    par->format = AV_PIX_FMT_NONE;
    par->hw_frames_ctx = hw_frames_ctx;
    // Initialize the buffersrc filter with the provided parameters
    ret = av_buffersrc_parameters_set(f->buffersrc_ctx, par);
    av_freep(&par);
    if (ret < 0)
      return ret;
  } else { // decoder out=sw
    Debug(1, "sw mode filter %p", hw_frames_ctx);
  }

  ret = avfilter_link(f->buffersrc_ctx, 0, cur->filter_ctx, cur->pad_idx);
  if (ret < 0) {
    Error("failed to link input filter");
    return ret;
  }

  cur = outputs, i = 0;
  snprintf(name, sizeof(name), "out_%d", i);
  ret = avfilter_graph_create_filter(&f->buffersink_ctx, avfilter_get_by_name("buffersink"), name, nullptr, nullptr, f->filter_graph);
  if (ret < 0) {
    Error("failed to create output filter: %d", i);
    return ret;
  } else {
    Debug(1, "Success creating output filter");
  }

  ret = avfilter_link(cur->filter_ctx, cur->pad_idx, f->buffersink_ctx, 0);
  if (ret < 0) {
    Error("failed to link output filter: %d", i);
    return ret;
  }

  // configure and validate the filter graph
  ret = avfilter_graph_config(f->filter_graph, nullptr);
  if (ret < 0) {
    Error("%s failed to config graph filter", __func__);
    return ret;
  } else {
    Debug(1, "%s success config graph filter %s", __func__, filters_desc);
  }

  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  return ret;
}

/* frame is the output from the model, not an image. Just raw info in the side data
 * file_frame is where we will stick the image with the bounding boxes.
 */
int Quadra_Yolo::process_roi(AVFrame *frame, AVFrame **filt_frame) {
  int i, num, ret;
  AVFrameSideData *sd = frame->side_data ? av_frame_get_side_data(frame, AV_FRAME_DATA_REGIONS_OF_INTEREST) : nullptr;
  AVFrameSideData *sd_roi_extra = frame->side_data ? av_frame_get_side_data( frame, AV_FRAME_DATA_NETINT_REGIONS_OF_INTEREST_EXTRA) : nullptr;

  Debug(4, "Filt %d frame pts %3" PRId64, ++filt_cnt, frame->pts);
	zm_dump_video_frame(frame, "Quadra: process_roi frame");
  if (!sd || !sd_roi_extra || sd->size == 0 || sd_roi_extra->size == 0) {
    *filt_frame = frame;
    if (*filt_frame == nullptr) {
      Error("cannot clone frame");
      return NIERROR(ENOMEM);
    }
    Debug(1, "no roi area in frame %d", filt_cnt);
    return 0;
  }

  AVFrame *input = nullptr;
  if (!use_hwframe) {
    // Allocates the frame, gets the image from hw
    ret = hwdl_filter.execute(frame, &input);
    if (ret < 0) {
      Error("cannot download hwframe");
      return ret;
    }
    zm_dump_video_frame(input, "Quadra: process_roi input");
  } else {
    input = frame;
  }

  AVRegionOfInterest *roi = (AVRegionOfInterest *)sd->data;
  AVRegionOfInterestNetintExtra *roi_extra = (AVRegionOfInterestNetintExtra *)sd_roi_extra->data;
  if ((sd->size % roi->self_size) ||
      (sd_roi_extra->size % roi_extra->self_size) ||
      (sd->size / roi->self_size !=
       sd_roi_extra->size / roi_extra->self_size)) {
    Error( "invalid roi side data");
    return ret = NIERROR(EINVAL);
  }
  num = sd->size / roi->self_size;

  nlohmann::json detections = nlohmann::json::array();

  for (i = 0; i < num; i++) {
    //if (check_movement(roi[i], roi_extra[i])) {
      //continue;
    //}
    int cls = roi_extra[i].cls;

    std::array<int, 4> bbox = {roi[i].left, roi[i].top, roi[i].right, roi[i].bottom};
    detections.push_back({{"class_name", roi_class[cls]}, {"bbox", bbox}, {"score", roi_extra[i].prob}});

    std::string color;
    if (cls == 0) {
      color = "Blue";
    } else {
      color = "Red";
    }

    if (drawbox and drawbox_filter.filter_ctx) {
      Debug(1, "Drawing box");
      AVFrame *drawbox_output = nullptr;
      ret = draw_roi_box(input, &drawbox_output, roi[i], roi_extra[i], monitor->LabelSize());
      if (ret < 0) {
        Error("draw %d roi box failed", i);
      } else {
        if (input != frame) av_frame_free(&input);
        input = drawbox_output;
        zm_dump_video_frame(input, "Quadra: drawbox");
      }
#if 0
      std::string annotation = stringtf("%s %d%%", roi_class[roi_extra[i].cls], static_cast<int>(100*roi_extra[i].prob));
      Image img(output);
      img.Annotate(annotation.c_str(), Vector2(roi[i].left, roi[i].top), monitor->LabelSize(), kRGBWhite, kRGBTransparent);
#endif
    }  // end if drawbox

    if (drawtext and drawtext_filter.filter_ctx) {
      Debug(1, "Drawing text");
      AVFrame *drawtext_output = nullptr;

      ret = draw_text(input, &drawtext_output, stringtf("%s: %.1f%%", roi_class[cls], 100*roi_extra[i].prob),
          roi[i].left+1+monitor->LabelSize(), roi[i].top+1+monitor->LabelSize(), color.c_str());
      if (ret < 0) {
        Error("cannot drawtext %d %s", ret, av_make_error_string(ret).c_str());
      } else {
        if (input != frame) av_frame_free(&input);
        input = drawtext_output;
        zm_dump_video_frame(input, "Quadra: drawtext");
      }
    }  // end if drawtext
  }  // end foreach roi

  AVFrame *output = nullptr;
  // Allocates the frame, gets the image from hw
  if (use_hwframe) {
    if (!hwdl_filter.initialised) {
      if (!hwdl_filter.setup(this, "hwdownload,format=yuv420p", "", input->hw_frames_ctx, dec_ctx->pix_fmt))
        Warning("No hwdl");
    }
    //input->colorspace = AVCOL_SPC_UNSPECIFIED;
    //input->color_range = AVCOL_RANGE_UNSPECIFIED;
    //input->hw_frames_ctx = frame->hw_frames_ctx;
    //Debug(1, "hw_frames_ctx %p => %p", frame->hw_frames_ctx, input->hw_frames_ctx);
    zm_dump_video_frame(input, "Quadra: process_roi hwframe");
    ret = hwdl_filter.execute(input, &output);
    if (ret < 0) {
      Error("cannot download hwframe");
      output = input;
    } else {
      zm_dump_video_frame(output, "Quadra: process_roi output");
    }
  } else {
    output = input;
  }

  *filt_frame = output;
  if (input != frame and input != output) av_frame_free(&input);
  
  free(last_roi);
  free(last_roi_extra);
  AVRegionOfInterest* cur_roi = nullptr;
  AVRegionOfInterestNetintExtra *cur_roi_extra = nullptr;

  if (num > 0) {
    cur_roi = (AVRegionOfInterest *)av_malloc((int)(num * sizeof(AVRegionOfInterest)));
    cur_roi_extra = (AVRegionOfInterestNetintExtra *)av_malloc((int)(num * sizeof(AVRegionOfInterestNetintExtra)));
    for (i = 0; i < num; i++) {
      (cur_roi[i]).self_size = roi[i].self_size;
      (cur_roi[i]).top       = roi[i].top;
      (cur_roi[i]).bottom    = roi[i].bottom;
      (cur_roi[i]).left      = roi[i].left;
      (cur_roi[i]).right     = roi[i].right;
      (cur_roi[i]).qoffset   = qp_offset;
      cur_roi_extra[i].self_size = roi_extra[i].self_size;
      cur_roi_extra[i].cls       = roi_extra[i].cls;
      cur_roi_extra[i].prob      = roi_extra[i].prob;
    }
  }
  last_roi = cur_roi;
  last_roi_extra = cur_roi_extra;
  last_roi_count = num;

  return detections.size();
}

int Quadra_Yolo::draw_last_roi(std::shared_ptr<ZMPacket> packet) {
  AVFrame *in_frame = packet->in_frame.get();
  if (!in_frame) { return 1; }

  if (!last_roi_count) return 1;

  for (int i = 0; i < last_roi_count; i++) {
    AVFrame *output = av_frame_alloc();
    // TODO use RAII
    if (!output) {
      Error("cannot allocate output filter frame");
      return NIERROR(ENOMEM);
    }
    int ret = draw_roi_box(in_frame, &output, last_roi[i], last_roi_extra[i]);
    if (ret < 0) {
      Error("draw %d roi box failed", i);
      av_frame_free(&output);
      return ret;
    }
    zm_dump_video_frame(output, "Quadra: boxes");
    std::string annotation = stringtf("%s %d%%", roi_class[last_roi_extra[i].cls], static_cast<int>(100*last_roi_extra[i].prob));
    Image img(output);
    img.Annotate(annotation.c_str(), Vector2(last_roi[i].left, last_roi[i].top), monitor->LabelSize(), kRGBWhite, kRGBTransparent);

    if (in_frame != packet->in_frame.get()) {
      Debug(1, "Freeing input");
      av_frame_free(&in_frame);
    }
    in_frame = output;
  } // end foreach detection
  packet->ai_frame = av_frame_ptr(in_frame);
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

  int ret = model->ni_get_boxes(model_ctx, out->width, out->height, &roi_box, &roi_num);
  if (ret < 0) {
    Error( "failed to get roi.");
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
          roi_box[i].right, roi_box[i].class, roi_class[roi_box[i].class], roi_box[i].prob);
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
  for (i = 0, j = 0; i < roi_num; i++) {
    //if (roi_box[i].ai_class == 0 || roi_box[i].ai_class == 7) {
      roi[j].self_size = sizeof(*roi);
      roi[j].top       = roi_box[i].top;
      roi[j].bottom    = roi_box[i].bottom;
      roi[j].left      = roi_box[i].left;
      roi[j].right     = roi_box[i].right;
      roi[j].qoffset   = qp_offset;
      roi_extra[j].self_size = sizeof(*roi_extra);
      roi_extra[j].cls       = roi_box[i].ai_class;
      roi_extra[j].prob      = roi_box[i].prob;
      Debug(4, "roi %d: top %d, bottom %d, left %d, right %d, class %d prob %f qpo %d/%d",
          j, roi[j].top, roi[j].bottom, roi[j].left, roi[j].right,
          roi_extra[j].cls, roi_extra[j].prob, roi[j].qoffset.num, roi[j].qoffset.den);
      j++;
    //}
  }
  if (roi_box) free(roi_box);
  return roi_num;
}

int Quadra_Yolo::draw_text(AVFrame *input, AVFrame **output, const std::string &text, int x, int y, const std::string &colour) {
  if (!drawtext_filter.filter_ctx) {
    Error("drawtext filter not configured");
    return -1;
  }

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

  return drawtext_filter.execute(input, output);
}

#endif
