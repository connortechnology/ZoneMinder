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

#include "zm_monitor.h"

// VideoStore encodes from the packet's decoded frame (hw_frame or in_frame),
// not its Image. Analyse() used to drop those frames once a packet had an
// Image, so the event thread got packets with nothing to encode and every
// encoded recording came out audio-only.
TEST_CASE("Analysis keeps decoded frames an encoder will read", "[Monitor]") {
  SECTION("encoding needs them") {
    REQUIRE_FALSE(Monitor::CanReleaseFramesAfterAnalysis(Monitor::ENCODE, Monitor::RECORDING_ALWAYS));
    REQUIRE_FALSE(Monitor::CanReleaseFramesAfterAnalysis(Monitor::ENCODE, Monitor::RECORDING_ONMOTION));
  }

  SECTION("passthrough records the compressed packet") {
    REQUIRE(Monitor::CanReleaseFramesAfterAnalysis(Monitor::PASSTHROUGH, Monitor::RECORDING_ALWAYS));
    REQUIRE(Monitor::CanReleaseFramesAfterAnalysis(Monitor::PASSTHROUGH, Monitor::RECORDING_ONMOTION));
  }

  SECTION("nothing records") {
    REQUIRE(Monitor::CanReleaseFramesAfterAnalysis(Monitor::ENCODE, Monitor::RECORDING_NONE));
    REQUIRE(Monitor::CanReleaseFramesAfterAnalysis(Monitor::DISABLED, Monitor::RECORDING_ALWAYS));
  }
}

// The Quadra decoder has only a few spare output buffers, and every decoded
// frame Analyse() holds is one of them. Holding them through motion detection
// left the decoder blocked in send_packet waiting for one back (refs #123), so
// the frame is dropped once object detection is done with it.
TEST_CASE("Analysis returns Quadra frames once object detection is done", "[Monitor]") {
  av_frame_ptr frame{av_frame_alloc()};
  REQUIRE(frame);

  SECTION("no frame") {
    REQUIRE_FALSE(Monitor::CanReleaseDeviceFrameAfterAI(nullptr, Monitor::PASSTHROUGH,
                                                        Monitor::RECORDING_ALWAYS));
  }

  SECTION("CUDA motion detection reads a CUDA frame after object detection") {
    frame->format = AV_PIX_FMT_CUDA;
    REQUIRE_FALSE(Monitor::CanReleaseDeviceFrameAfterAI(frame.get(), Monitor::PASSTHROUGH,
                                                        Monitor::RECORDING_ALWAYS));
  }

#if HAVE_QUADRA
  SECTION("a Quadra frame nothing else reads") {
    frame->format = AV_PIX_FMT_NI_QUAD;
    REQUIRE(Monitor::CanReleaseDeviceFrameAfterAI(frame.get(), Monitor::PASSTHROUGH,
                                                  Monitor::RECORDING_ALWAYS));
    REQUIRE(Monitor::CanReleaseDeviceFrameAfterAI(frame.get(), Monitor::ENCODE,
                                                  Monitor::RECORDING_NONE));
  }

  SECTION("a Quadra frame the encoder will read") {
    frame->format = AV_PIX_FMT_NI_QUAD;
    REQUIRE_FALSE(Monitor::CanReleaseDeviceFrameAfterAI(frame.get(), Monitor::ENCODE,
                                                        Monitor::RECORDING_ALWAYS));
    REQUIRE_FALSE(Monitor::CanReleaseDeviceFrameAfterAI(frame.get(), Monitor::ENCODE,
                                                        Monitor::RECORDING_ONMOTION));
  }
#endif
}
