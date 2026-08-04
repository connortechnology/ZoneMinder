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

#include "zm_ffmpeg_camera.h"
#include "zm_time.h"

// ComputeRealtimePace() is the pure decision behind FfmpegCamera's "realtime=1"
// (ffmpeg -re style) pacing: given the current packet timestamp, the active
// anchor timestamp, the wall-clock time elapsed since that anchor, and the
// discontinuity cap, it decides whether to re-anchor and how long to sleep.
static const Microseconds kCap = std::chrono::duration_cast<Microseconds>(Seconds(10));

TEST_CASE("ComputeRealtimePace: sleeps the full interval when no time has elapsed") {
  // 40ms into the stream with zero wall-clock elapsed -> wait the whole 40ms.
  RealtimePaceDecision d = ComputeRealtimePace(40000, 0, Microseconds(0), kCap);
  REQUIRE_FALSE(d.reanchor);
  REQUIRE(d.sleep == Microseconds(40000));
}

TEST_CASE("ComputeRealtimePace: sleeps only the remaining interval when partially elapsed") {
  // Target is 40ms ahead of the anchor, 15ms has already passed -> sleep 25ms.
  RealtimePaceDecision d = ComputeRealtimePace(40000, 0, Microseconds(15000), kCap);
  REQUIRE_FALSE(d.reanchor);
  REQUIRE(d.sleep == Microseconds(25000));
}

TEST_CASE("ComputeRealtimePace: non-zero anchor only the delta matters") {
  // Anchor at 1s, packet at 1.040s, 10ms elapsed -> 30ms remaining.
  RealtimePaceDecision d = ComputeRealtimePace(1040000, 1000000, Microseconds(10000), kCap);
  REQUIRE_FALSE(d.reanchor);
  REQUIRE(d.sleep == Microseconds(30000));
}

TEST_CASE("ComputeRealtimePace: behind schedule delivers immediately without re-anchoring") {
  // Only 40ms into the stream but 100ms of wall-clock has passed: we are behind,
  // so deliver now (no sleep) and keep the anchor so we can catch back up.
  RealtimePaceDecision d = ComputeRealtimePace(40000, 0, Microseconds(100000), kCap);
  REQUIRE_FALSE(d.reanchor);
  REQUIRE(d.sleep == Microseconds(0));
}

TEST_CASE("ComputeRealtimePace: exactly on schedule does not sleep") {
  RealtimePaceDecision d = ComputeRealtimePace(40000, 0, Microseconds(40000), kCap);
  REQUIRE_FALSE(d.reanchor);
  REQUIRE(d.sleep == Microseconds(0));
}

TEST_CASE("ComputeRealtimePace: backward timestamp re-anchors instead of sleeping") {
  // A timestamp before the anchor (discontinuity/reset) must never produce a
  // negative sleep; it re-anchors so pacing restarts from the new position.
  RealtimePaceDecision d = ComputeRealtimePace(500000, 1000000, Microseconds(0), kCap);
  REQUIRE(d.reanchor);
  REQUIRE(d.sleep == Microseconds(0));
}

TEST_CASE("ComputeRealtimePace: gap beyond the cap re-anchors instead of stalling") {
  // 30s ahead of schedule with a 10s cap is treated as a discontinuity, not a
  // genuine 30s frame interval, so we re-anchor rather than sleep 30s.
  RealtimePaceDecision d =
    ComputeRealtimePace(30 * 1000000LL, 0, Microseconds(0), kCap);
  REQUIRE(d.reanchor);
  REQUIRE(d.sleep == Microseconds(0));
}

TEST_CASE("ComputeRealtimePace: a delay right at the cap still sleeps") {
  // Boundary: delay == cap is allowed (only delays strictly greater re-anchor).
  RealtimePaceDecision d = ComputeRealtimePace(10 * 1000000LL, 0, Microseconds(0), kCap);
  REQUIRE_FALSE(d.reanchor);
  REQUIRE(d.sleep == kCap);
}

// SeekToStart() rewinds an input for loop-on-EOF playback.
//
// The case that matters is a raw elementary stream (.h264/.265): no container,
// no index, and no timestamps at all - start_time and duration both come back
// AV_NOPTS_VALUE. Every timestamp-based seek ffmpeg offers fails on those with a
// bare -1, which av_strerror renders as "Operation not permitted" because
// AVERROR(EPERM) is also -1. That reads like a filesystem permissions problem
// and is not one. Only a byte seek rewinds such a stream.

namespace {

// Encode a few frames straight to a file with no muxer, producing a raw
// elementary stream. Returns false if this ffmpeg build lacks the encoder.
bool WriteElementaryStream(const std::string &path, const AVCodec *codec) {
  if (!codec) return false;

  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx) return false;
  ctx->width = 64;
  ctx->height = 64;
  ctx->time_base = AVRational{1, 25};
  ctx->framerate = AVRational{25, 1};
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->gop_size = 2;
  ctx->bit_rate = 200000;

  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    avcodec_free_context(&ctx);
    return false;
  }

  AVFrame *frame = av_frame_alloc();
  frame->width = ctx->width;
  frame->height = ctx->height;
  frame->format = ctx->pix_fmt;
  FILE *out = nullptr;
  bool ok = (av_frame_get_buffer(frame, 32) >= 0);
  if (ok) {
    out = fopen(path.c_str(), "wb");
    ok = (out != nullptr);
  }

  if (ok) {
    AVPacket *pkt = av_packet_alloc();
    for (int i = 0; i < 25; i++) {
      av_frame_make_writable(frame);
      // A moving luma ramp so successive frames genuinely differ.
      for (int y = 0; y < ctx->height; y++) {
        memset(frame->data[0] + y * frame->linesize[0], (i * 8 + y) & 0xff, ctx->width);
      }
      for (int y = 0; y < ctx->height / 2; y++) {
        memset(frame->data[1] + y * frame->linesize[1], 128, ctx->width / 2);
        memset(frame->data[2] + y * frame->linesize[2], 128, ctx->width / 2);
      }
      frame->pts = i;
      if (avcodec_send_frame(ctx, frame) >= 0) {
        while (avcodec_receive_packet(ctx, pkt) >= 0) {
          fwrite(pkt->data, 1, pkt->size, out);
          av_packet_unref(pkt);
        }
      }
    }
    avcodec_send_frame(ctx, nullptr);
    while (avcodec_receive_packet(ctx, pkt) >= 0) {
      fwrite(pkt->data, 1, pkt->size, out);
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    fclose(out);
  }

  av_frame_free(&frame);
  avcodec_free_context(&ctx);
  return ok;
}

// Advance into the file so a rewind has something to undo.
int ReadSome(AVFormatContext *ctx, int want) {
  AVPacket *pkt = av_packet_alloc();
  int read = 0;
  while (read < want && av_read_frame(ctx, pkt) >= 0) {
    av_packet_unref(pkt);
    read++;
  }
  av_packet_free(&pkt);
  return read;
}

}  // namespace

TEST_CASE("SeekToStart: rewinds a raw stream that has no timestamps to seek by") {
  // libx264 by name rather than by codec id: resolving H264 generically can land
  // on a hardware encoder that will not open in a test environment.
  const std::string path = "/tmp/zm_seektostart_raw.h264";
  if (!WriteElementaryStream(path, avcodec_find_encoder_by_name("libx264"))) {
    WARN("no libx264 encoder in this ffmpeg build; skipping raw-stream seek test");
    return;
  }

  AVFormatContext *ctx = nullptr;
  REQUIRE(avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) >= 0);
  avformat_find_stream_info(ctx, nullptr);

  // This fixture must actually be the no-timestamp case, otherwise the test
  // silently stops covering the bug it exists for.
  REQUIRE(ctx->duration == AV_NOPTS_VALUE);

  REQUIRE(ReadSome(ctx, 10) > 0);
  REQUIRE(avio_tell(ctx->pb) > 0);

  // The two timestamp seeks the old code relied on both fail here.
  REQUIRE(avformat_seek_file(ctx, -1, INT64_MIN, 0, INT64_MAX, AVSEEK_FLAG_BACKWARD) < 0);
  REQUIRE(av_seek_frame(ctx, -1, 0, AVSEEK_FLAG_BACKWARD) < 0);

  // SeekToStart falls through to the byte seek and rewinds.
  REQUIRE(SeekToStart(ctx) >= 0);
  REQUIRE(avio_tell(ctx->pb) == 0);

  // And the stream is genuinely usable again, not merely repositioned.
  AVPacket *pkt = av_packet_alloc();
  REQUIRE(av_read_frame(ctx, pkt) >= 0);
  REQUIRE(pkt->size > 0);
  av_packet_free(&pkt);

  avformat_close_input(&ctx);
  std::remove(path.c_str());
}

TEST_CASE("SeekToStart: still rewinds when timestamp seeking does work") {
  // mpeg1video in a raw stream gets a duration estimated from bitrate, so the
  // timestamp seek succeeds and SeekToStart returns on its first branch. Guards
  // the byte-seek fallback against regressing the ordinary path.
  const std::string path = "/tmp/zm_seektostart_ts.m1v";
  if (!WriteElementaryStream(path, avcodec_find_encoder(AV_CODEC_ID_MPEG1VIDEO))) {
    WARN("no mpeg1video encoder in this ffmpeg build; skipping");
    return;
  }

  AVFormatContext *ctx = nullptr;
  REQUIRE(avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) >= 0);
  avformat_find_stream_info(ctx, nullptr);

  REQUIRE(ReadSome(ctx, 10) > 0);
  REQUIRE(avformat_seek_file(ctx, -1, INT64_MIN, 0, INT64_MAX, AVSEEK_FLAG_BACKWARD) >= 0);

  // Drain to EOF, which is the state loop-on-EOF actually calls this from.
  ReadSome(ctx, 1000);
  REQUIRE(SeekToStart(ctx) >= 0);

  AVPacket *pkt = av_packet_alloc();
  REQUIRE(av_read_frame(ctx, pkt) >= 0);
  av_packet_free(&pkt);

  avformat_close_input(&ctx);
  std::remove(path.c_str());
}
