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

// A frame with no score only moves the state machine out of ALARM when it
// tells us something: motion detection looked at it and found nothing, or the
// monitor is not doing motion detection at all. Frames motion detection
// skipped (motion_frame_skip) or used to seed the reference report -1 and must
// not end an alarm. Analyse() used to leave the value it checked at -1 for
// every frame, so an Analysing=Always monitor never left ALARM (refs #128).
TEST_CASE("Which quiet frames can end an alarm", "[Monitor]") {
  SECTION("motion detection ran and found nothing") {
    REQUIRE(Monitor::QuietFrameCounts(0, Monitor::ANALYSING_ALWAYS));
  }

  SECTION("motion detection did not run on this frame") {
    REQUIRE_FALSE(Monitor::QuietFrameCounts(-1, Monitor::ANALYSING_ALWAYS));
  }

  SECTION("no motion detection at all") {
    REQUIRE(Monitor::QuietFrameCounts(-1, Monitor::ANALYSING_NONE));
  }
}

// Motion detection runs on a frame MotionFrameWidth wide, with the height
// following the capture's aspect ratio. 0, or a width that is not smaller than
// the capture, means the capture's own size.
TEST_CASE("Motion frame size follows the capture aspect ratio", "[Monitor]") {
  using Size = std::pair<unsigned int, unsigned int>;

  SECTION("0 means the capture size") {
    REQUIRE(Monitor::MotionFrameSize(0, 3840, 2160) == Size(3840, 2160));
  }

  SECTION("never larger than the capture") {
    REQUIRE(Monitor::MotionFrameSize(3840, 3840, 2160) == Size(3840, 2160));
    REQUIRE(Monitor::MotionFrameSize(5000, 3840, 2160) == Size(3840, 2160));
  }

  SECTION("scaled down") {
    REQUIRE(Monitor::MotionFrameSize(1280, 3840, 2160) == Size(1280, 720));
    REQUIRE(Monitor::MotionFrameSize(640, 1920, 1080) == Size(640, 360));
    REQUIRE(Monitor::MotionFrameSize(540, 1080, 1920) == Size(540, 960));
  }

  SECTION("rounded down to even") {
    // 1278 * 2160 / 3840 = 718.9, so 719 before rounding to even.
    REQUIRE(Monitor::MotionFrameSize(1279, 3840, 2160) == Size(1278, 718));
    REQUIRE(Monitor::MotionFrameSize(641, 1920, 1080) == Size(640, 360));
  }

  SECTION("never smaller than 2x2") {
    REQUIRE(Monitor::MotionFrameSize(1, 3840, 2160) == Size(2, 2));
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

// Motion detection runs at MotionFrameWidth, so a zone's alarm image is at the
// motion size while the analysis image is at the capture size. Overlaying one
// onto the other panicked and took zmc down at every alarm.
TEST_CASE("A motion-sized alarm image overlays onto the capture-sized analysis image", "[Monitor]") {
  Image analysis(64, 36, ZM_COLOUR_GRAY8, ZM_SUBPIX_ORDER_NONE);
  analysis.Clear();
  Image alarm(16, 9, ZM_COLOUR_GRAY8, ZM_SUBPIX_ORDER_NONE);
  alarm.Clear();
  // An alarmed block in the top left quarter of the motion frame.
  for (unsigned int y = 1; y < 4; y++)
    for (unsigned int x = 1; x < 7; x++) *alarm.Buffer(x, y) = 255;

  REQUIRE(Monitor::OverlayAlarmImage(analysis, alarm, kRGBWhite, 255));
  REQUIRE(analysis.Width() == 64);
  REQUIRE(analysis.Height() == 36);
  // The block, four times larger, is painted; the far corner is not.
  CHECK(*analysis.Buffer(12, 8) != 0);
  CHECK(*analysis.Buffer(20, 10) != 0);
  CHECK(*analysis.Buffer(60, 32) == 0);
}

TEST_CASE("An alarm image already at the analysis size overlays unscaled", "[Monitor]") {
  Image analysis(32, 18, ZM_COLOUR_GRAY8, ZM_SUBPIX_ORDER_NONE);
  analysis.Clear();
  Image alarm(32, 18, ZM_COLOUR_GRAY8, ZM_SUBPIX_ORDER_NONE);
  alarm.Clear();
  *alarm.Buffer(5, 5) = 255;

  REQUIRE(Monitor::OverlayAlarmImage(analysis, alarm, kRGBWhite, 255));
  CHECK(*analysis.Buffer(5, 5) != 0);
  CHECK(*analysis.Buffer(6, 5) == 0);
}
