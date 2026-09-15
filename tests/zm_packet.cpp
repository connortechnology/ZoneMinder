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

#include "zm_catch2.h"

#include "zm_packet.h"
#include "zm_ffmpeg.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

namespace {
// A frame with real buffers, so a double free would actually corrupt the heap
// rather than passing unnoticed.
AVFrame *make_frame(int w = 64, int h = 48) {
  AVFrame *f = av_frame_alloc();
  REQUIRE(f != nullptr);
  f->format = AV_PIX_FMT_YUV420P;
  f->width = w;
  f->height = h;
  REQUIRE(av_frame_get_buffer(f, 32) == 0);
  return f;
}
}  // namespace

TEST_CASE("ZMPacket::set_ai_frame ownership", "[packet]") {
  SECTION("takes ownership of a frame the packet does not already hold") {
    ZMPacket packet;
    AVFrame *frame = make_frame();
    packet.set_ai_frame(frame);
    CHECK(packet.ai_frame.get() == frame);
    // Destruction frees it exactly once. Running under ASAN proves it.
  }

  SECTION("does not take a second ownership of the packet's own hw_frame") {
    // The real bug: Quadra_Yolo::process_roi handed back packet->hw_frame on
    // any frame with no detections, and set_ai_frame then owned it too, so
    // ~ZMPacket freed one AVFrame twice and aborted inside _int_free.
    ZMPacket packet;
    AVFrame *frame = make_frame();
    // hw_frame counts against the device-frame gauge, so it takes the
    // device_frame_ptr deleter rather than the plain one.
    packet.hw_frame = device_frame_ptr{frame};

    packet.set_ai_frame(frame);

    CHECK(packet.ai_frame.get() != nullptr);
    CHECK(packet.ai_frame.get() != packet.hw_frame.get());
    // Both still refer to the same picture.
    CHECK(packet.ai_frame->data[0] == packet.hw_frame->data[0]);
  }

  SECTION("does not take a second ownership of in_frame or out_frame") {
    {
      ZMPacket packet;
      AVFrame *frame = make_frame();
      packet.in_frame = av_frame_ptr{frame};
      packet.set_ai_frame(frame);
      CHECK(packet.ai_frame.get() != packet.in_frame.get());
    }
    {
      ZMPacket packet;
      AVFrame *frame = make_frame();
      packet.out_frame = av_frame_ptr{frame};
      packet.set_ai_frame(frame);
      CHECK(packet.ai_frame.get() != packet.out_frame.get());
    }
  }

  SECTION("a null frame is not a double free either") {
    ZMPacket packet;
    packet.set_ai_frame(nullptr);
    CHECK(packet.ai_frame.get() == nullptr);
  }
}
