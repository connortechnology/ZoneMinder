// Measures what a hardware jpeg encoder is actually worth, by driving
// libavcodec directly rather than through ffmpeg's filter CLI.
//
// Standalone, like tests/av1_framing_harness.cpp: not built by the test suite,
// because it needs a VAAPI device and a video file. Build and run it by hand.
//
// Three paths, all from the same h264 file:
//
//   software decode -> software mjpeg           what ZoneMinder does today
//   vaapi decode -> download -> software mjpeg  ZoneMinder with hw decoding on
//   vaapi decode -> mjpeg_vaapi                 never leaves the device
//
// Measured on one Intel iGPU (iHD), 300 frames of 1080p:
//
//   software decode + software mjpeg          8.27 ms/frame
//   vaapi decode + download + sw mjpeg        8.86 ms/frame   +7%
//   vaapi decode + mjpeg_vaapi (resident)     0.39 ms/frame   -95%
//
// The middle row is the point. Hardware decoding on its own is a net loss:
// av_hwframe_transfer_data costs more than the hardware decode saves. The win
// is not in any one leg, it is in never crossing the bus, which is why this
// only pays off as a whole-pipeline change.
//
// Reports process CPU time, not wall clock, because the question is how many
// cameras fit on a box. It does not measure GPU load, which is its own ceiling
// and is what made the NetInt Quadra jpeg encoder not worth using.
//
// Two things this harness learned the hard way, both of which reported
// plausible numbers while measuring nothing:
//   - a path that fails still records a CPU time, so the frame count is
//     checked and a jpeg from each path can be dumped and inspected
//   - VAAPI h264 decode is 4:2:0 only; a yuv444p input silently never offers
//     the hardware format, so get_format reports what it was actually offered
//
// Build:
//   g++ -O2 -std=c++17 tests/jpeg_hwaccel_bench.cpp -o /tmp/jpeg_hwaccel_bench \
//       $(pkg-config --cflags --libs libavformat libavcodec libavutil libswscale)
// Run:
//   /tmp/jpeg_hwaccel_bench <file.mp4> [frames] [/dev/dri/renderD128]
//   BENCH_DUMP=/tmp /tmp/jpeg_hwaccel_bench <file.mp4>   # also writes modeN.jpg
//
// Make a suitable input with:
//   ffmpeg -f lavfi -i testsrc=size=1920x1080:rate=25:duration=12 \
//          -c:v libx264 -pix_fmt yuv420p -g 25 /tmp/src420.mp4

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace {

double CpuSeconds() {
  timespec ts{};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

AVPixelFormat gWanted = AV_PIX_FMT_NONE;
bool gGotWanted = false;
AVPixelFormat PickFormat(AVCodecContext *, const AVPixelFormat *fmts) {
  for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
    if (*p == gWanted) { gGotWanted = true; return *p; }
  }
  // Say what was on offer, then fall back to software rather than aborting.
  fprintf(stderr, "  wanted %s, decoder offered:", av_get_pix_fmt_name(gWanted));
  for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
    fprintf(stderr, " %s", av_get_pix_fmt_name(*p));
  fprintf(stderr, "\n");
  return fmts[0];
}

struct Input {
  AVFormatContext *fmt = nullptr;
  AVCodecContext *dec = nullptr;
  int stream = -1;

  bool Open(const std::string &path, AVBufferRef *hw_device) {
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
    stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream < 0) return false;
    AVCodecParameters *par = fmt->streams[stream]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, par);
    if (hw_device) {
      dec->hw_device_ctx = av_buffer_ref(hw_device);
      gWanted = AV_PIX_FMT_VAAPI;
      gGotWanted = false;
      dec->get_format = PickFormat;
    }
    return avcodec_open2(dec, codec, nullptr) >= 0;
  }
  ~Input() {
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
  }
};

// Software mjpeg encoder over yuvj420p in system memory.
struct SoftwareJpeg {
  AVCodecContext *ctx = nullptr;
  SwsContext *sws = nullptr;
  AVFrame *staging = nullptr;

  bool Open(int w, int h) {
    const AVCodec *codec = avcodec_find_encoder_by_name("mjpeg");
    if (!codec) return false;
    ctx = avcodec_alloc_context3(codec);
    ctx->width = w; ctx->height = h;
    ctx->time_base = AVRational{1, 25};
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = 8 * FF_QP2LAMBDA;
    if (avcodec_open2(ctx, codec, nullptr) < 0) return false;
    staging = av_frame_alloc();
    staging->width = w; staging->height = h; staging->format = AV_PIX_FMT_YUVJ420P;
    return av_frame_get_buffer(staging, 0) >= 0;
  }
  // frame is in system memory, any pixel format
  bool Encode(AVFrame *frame, AVPacket *pkt) {
    if (!sws) {
      sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                           ctx->width, ctx->height, AV_PIX_FMT_YUVJ420P,
                           SWS_BICUBIC, nullptr, nullptr, nullptr);
      if (!sws) return false;
    }
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
              staging->data, staging->linesize);
    staging->quality = ctx->global_quality;
    if (avcodec_send_frame(ctx, staging) < 0) return false;
    return avcodec_receive_packet(ctx, pkt) >= 0;
  }
  ~SoftwareJpeg() {
    if (sws) sws_freeContext(sws);
    if (staging) av_frame_free(&staging);
    if (ctx) avcodec_free_context(&ctx);
  }
};

// mjpeg_vaapi over frames that are already on the device.
struct VaapiJpeg {
  AVCodecContext *ctx = nullptr;

  bool Open(int w, int h, AVBufferRef *frames_ctx) {
    const AVCodec *codec = avcodec_find_encoder_by_name("mjpeg_vaapi");
    if (!codec) { fprintf(stderr, "  mjpeg_vaapi not built into this ffmpeg\n"); return false; }
    ctx = avcodec_alloc_context3(codec);
    ctx->width = w; ctx->height = h;
    ctx->time_base = AVRational{1, 25};
    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    // Share the decoder's frame pool so nothing is copied between them.
    ctx->hw_frames_ctx = av_buffer_ref(frames_ctx);
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    ctx->global_quality = 8 * FF_QP2LAMBDA;
    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
      char err[256]; av_strerror(ret, err, sizeof(err));
      fprintf(stderr, "  mjpeg_vaapi open failed: %s\n", err);
      return false;
    }
    return true;
  }
  bool Encode(AVFrame *device_frame, AVPacket *pkt) {
    device_frame->quality = ctx->global_quality;
    if (avcodec_send_frame(ctx, device_frame) < 0) return false;
    return avcodec_receive_packet(ctx, pkt) >= 0;
  }
  ~VaapiJpeg() { if (ctx) avcodec_free_context(&ctx); }
};

struct Result { int frames = 0; double cpu = 0; long bytes = 0; bool ok = false; };

// mode: 0 sw decode + sw jpeg, 1 hw decode + download + sw jpeg, 2 hw decode + vaapi jpeg
Result Run(const std::string &path, int want, int mode, const char *device) {
  Result r;
  AVBufferRef *hw_device = nullptr;
  if (mode != 0 && av_hwdevice_ctx_create(&hw_device, AV_HWDEVICE_TYPE_VAAPI, device, nullptr, 0) < 0) {
    fprintf(stderr, "  cannot open vaapi device %s\n", device);
    return r;
  }

  Input in;
  if (!in.Open(path, hw_device)) { if (hw_device) av_buffer_unref(&hw_device); return r; }

  SoftwareJpeg sw;
  VaapiJpeg va;
  bool enc_open = false;

  AVPacket *pkt = av_packet_alloc();
  AVPacket *out = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  AVFrame *downloaded = av_frame_alloc();

  const double t0 = CpuSeconds();

  bool draining = false;
  while (r.frames < want) {
    if (!draining) {
      if (av_read_frame(in.fmt, pkt) < 0) {
        draining = true;
        avcodec_send_packet(in.dec, nullptr);   // flush
      } else {
        if (pkt->stream_index != in.stream) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(in.dec, pkt) < 0) { av_packet_unref(pkt); continue; }
        av_packet_unref(pkt);
      }
    }

    while (r.frames < want && avcodec_receive_frame(in.dec, frame) >= 0) {
      AVFrame *to_encode = frame;

      if (mode == 1) {  // bring it down to system memory first
        av_frame_unref(downloaded);
        if (av_hwframe_transfer_data(downloaded, frame, 0) < 0) { av_frame_unref(frame); goto done; }
        to_encode = downloaded;
      }

      if (!enc_open) {
        if (mode == 2) {
          if (!in.dec->hw_frames_ctx) { fprintf(stderr, "  decoder produced no hw frames context\n"); goto done; }
          if (!va.Open(to_encode->width, to_encode->height, in.dec->hw_frames_ctx)) goto done;
        } else {
          if (!sw.Open(to_encode->width, to_encode->height)) goto done;
        }
        enc_open = true;
      }

      bool got = (mode == 2) ? va.Encode(to_encode, out) : sw.Encode(to_encode, out);
      if (got) {
        r.bytes += out->size;
        if (r.bytes == out->size && getenv("BENCH_DUMP")) {
          char name[256];
          snprintf(name, sizeof(name), "%s/mode%d.jpg", getenv("BENCH_DUMP"), mode);
          if (FILE *f = fopen(name, "wb")) { fwrite(out->data, 1, out->size, f); fclose(f); }
        }
        av_packet_unref(out);
      }
      r.frames++;
      av_frame_unref(frame);
    }
    if (draining) break;
  }
done:
  r.cpu = CpuSeconds() - t0;
  r.ok = r.frames == want;

  av_frame_free(&downloaded);
  av_frame_free(&frame);
  av_packet_free(&out);
  av_packet_free(&pkt);
  if (hw_device) av_buffer_unref(&hw_device);
  return r;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <file> [frames] [device]\n", argv[0]); return 2; }
  const std::string path = argv[1];
  const int frames = (argc > 2) ? atoi(argv[2]) : 300;
  const char *device = (argc > 3) ? argv[3] : "/dev/dri/renderD128";

  av_log_set_level(AV_LOG_ERROR);

  struct { const char *name; int mode; } paths[] = {
    {"software decode + software mjpeg", 0},
    {"vaapi decode + download + sw mjpeg", 1},
    {"vaapi decode + mjpeg_vaapi (resident)", 2},
  };

  printf("%d frames from %s\n\n", frames, path.c_str());
  double baseline = 0;
  for (auto &p : paths) {
    Result r = Run(path, frames, p.mode, device);
    if (!r.ok) { printf("  %-40s FAILED after %d frames\n", p.name, r.frames); continue; }
    if (p.mode == 0) baseline = r.cpu;
    printf("  %-40s cpu %6.2fs  %6.2f ms/frame  %6.1f KB/frame", p.name, r.cpu,
           r.cpu * 1000 / r.frames, r.bytes / 1024.0 / r.frames);
    if (baseline > 0 && p.mode != 0) printf("   %+.0f%% cpu", (r.cpu - baseline) / baseline * 100);
    printf("\n");
  }
  return 0;
}
