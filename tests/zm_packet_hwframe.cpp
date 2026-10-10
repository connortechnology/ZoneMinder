/*
 * This file is part of the ZoneMinder Project. See AUTHORS file for Copyright information
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "zm_catch2.h"

#include "zm_ffmpeg.h"
#include "zm_packet.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

// receive_frame and transfer_hwframe report kDownloadedFromDevice when they
// copied a frame from the card into system memory, and 1 for any other
// success. Monitor counts the crossings from that return value, so a frame
// that never touched the card must not report one, and a frame that did must.
//
// The device cases need a VAAPI render node. Without one they skip rather
// than fail, so they run in CI as the software cases only.

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;

AVBufferRef *create_vaapi_device() {
  AVBufferRef *device = nullptr;
  if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0)
    return nullptr;
  return device;
}

av_frame_ptr software_frame(AVPixelFormat format) {
  av_frame_ptr frame{av_frame_alloc()};
  frame->format = format;
  frame->width = kWidth;
  frame->height = kHeight;
  REQUIRE(av_frame_get_buffer(frame.get(), 0) == 0);
  // Mid-grey in every plane, so the frame encodes to something valid.
  for (int plane = 0; plane < AV_NUM_DATA_POINTERS and frame->data[plane]; plane++) {
    const int rows = plane ? AV_CEIL_RSHIFT(kHeight, 1) : kHeight;
    memset(frame->data[plane], 128, static_cast<size_t>(frame->linesize[plane]) * rows);
  }
  return frame;
}

AVPixelFormat pick_vaapi(AVCodecContext *ctx, const AVPixelFormat *formats) {
  for (const AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
    if (*p == AV_PIX_FMT_VAAPI) {
      *static_cast<bool *>(ctx->opaque) = true;
      return *p;
    }
  }
  return formats[0];
}

}  // namespace

TEST_CASE("transfer_hwframe: a software frame is not a download", "[ffmpeg][hwaccel]") {
  AVCodecContext *ctx = avcodec_alloc_context3(nullptr);
  ctx->sw_pix_fmt = AV_PIX_FMT_NV12;

  ZMPacket packet;
  packet.in_frame = software_frame(AV_PIX_FMT_YUV420P);

  REQUIRE(packet.transfer_hwframe(ctx) == 1);
  REQUIRE(packet.hw_frame == nullptr);

  avcodec_free_context(&ctx);
}

TEST_CASE("transfer_hwframe: a frame on the card is one download", "[ffmpeg][hwaccel]") {
  AVBufferRef *device = create_vaapi_device();
  if (!device) {
    WARN("no VAAPI device; skipping");
    return;
  }
  AVBufferRef *frames = av_hwframe_ctx_alloc(device);
  auto *frames_ctx = reinterpret_cast<AVHWFramesContext *>(frames->data);
  frames_ctx->format = AV_PIX_FMT_VAAPI;
  frames_ctx->sw_format = AV_PIX_FMT_NV12;
  frames_ctx->width = kWidth;
  frames_ctx->height = kHeight;
  frames_ctx->initial_pool_size = 2;
  REQUIRE(av_hwframe_ctx_init(frames) == 0);

  AVCodecContext *ctx = avcodec_alloc_context3(nullptr);
  ctx->sw_pix_fmt = AV_PIX_FMT_NV12;

  ZMPacket packet;
  packet.in_frame = av_frame_ptr{av_frame_alloc()};
  REQUIRE(av_hwframe_get_buffer(frames, packet.in_frame.get(), 0) == 0);

  REQUIRE(packet.transfer_hwframe(ctx) == ZMPacket::kDownloadedFromDevice);
  REQUIRE(packet.hw_frame != nullptr);
  REQUIRE(packet.in_frame->hw_frames_ctx == nullptr);
  REQUIRE(packet.in_frame->format == AV_PIX_FMT_NV12);

  // Already in system memory: asking again must not count a second crossing.
  REQUIRE(packet.transfer_hwframe(ctx) == 1);

  packet.in_frame = nullptr;
  packet.hw_frame = nullptr;
  avcodec_free_context(&ctx);
  av_buffer_unref(&frames);
  av_buffer_unref(&device);
}

TEST_CASE("receive_frame: download reported only for hardware decode", "[ffmpeg][hwaccel]") {
  const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
  if (!encoder or !decoder) {
    WARN("no mjpeg codec in this ffmpeg build; skipping");
    return;
  }

  AVCodecContext *enc = avcodec_alloc_context3(encoder);
  enc->width = kWidth;
  enc->height = kHeight;
  enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
  enc->time_base = {1, 25};
  REQUIRE(avcodec_open2(enc, encoder, nullptr) == 0);
  av_frame_ptr source = software_frame(AV_PIX_FMT_YUVJ420P);
  REQUIRE(avcodec_send_frame(enc, source.get()) == 0);
  av_packet_ptr jpeg{av_packet_alloc()};
  REQUIRE(avcodec_receive_packet(enc, jpeg.get()) == 0);
  avcodec_free_context(&enc);

  auto decode = [&](AVBufferRef *device, bool *chose_vaapi) {
    AVCodecContext *dec = avcodec_alloc_context3(decoder);
    if (device) {
      dec->hw_device_ctx = av_buffer_ref(device);
      dec->opaque = chose_vaapi;
      dec->get_format = pick_vaapi;
    }
    REQUIRE(avcodec_open2(dec, decoder, nullptr) == 0);
    ZMPacket packet;
    av_packet_ref(packet.packet.get(), jpeg.get());
    REQUIRE(packet.send_packet(dec) == 1);
    int ret = packet.receive_frame(dec);
    if (ret == 0) {
      // Some decoders hold the frame until drained.
      avcodec_send_packet(dec, nullptr);
      ret = packet.receive_frame(dec);
    }
    REQUIRE(packet.in_frame != nullptr);
    REQUIRE(packet.in_frame->hw_frames_ctx == nullptr);
    avcodec_free_context(&dec);
    return ret;
  };

  SECTION("software decode") {
    REQUIRE(decode(nullptr, nullptr) == 1);
  }

  SECTION("VAAPI decode") {
    AVBufferRef *device = create_vaapi_device();
    if (!device) {
      WARN("no VAAPI device; skipping");
      return;
    }
    bool chose_vaapi = false;
    const int ret = decode(device, &chose_vaapi);
    av_buffer_unref(&device);
    if (!chose_vaapi) {
      WARN("VAAPI does not decode mjpeg on this device; skipping");
      return;
    }
    REQUIRE(ret == ZMPacket::kDownloadedFromDevice);
  }
}
