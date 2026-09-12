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

#include "zm_ffmpeg.h"

namespace {

// encoder_share_pool only ever passes the pointer through or drops it, so a
// stand-in is enough; nothing dereferences it.
AVBufferRef *const kDecoderPool = reinterpret_cast<AVBufferRef *>(0x1000);

}  // namespace

TEST_CASE("encoder_share_pool", "[ffmpeg]") {
  SECTION("shares the decoder pool when frames go straight to the encoder") {
    REQUIRE(encoder_share_pool(kDecoderPool, false) == kDecoderPool);
  }

  // The regression: object detection encodes a software copy with boxes drawn
  // on it, so every frame needs av_hwframe_get_buffer(), which fails with
  // EINVAL against a pool the decoder owns and ends recording for the event.
  SECTION("withholds the pool when frames are rewritten in software") {
    REQUIRE(encoder_share_pool(kDecoderPool, true) == nullptr);
  }

  SECTION("has nothing to share when the decoder has no pool") {
    REQUIRE(encoder_share_pool(nullptr, false) == nullptr);
    REQUIRE(encoder_share_pool(nullptr, true) == nullptr);
  }
}

TEST_CASE("software_frames_expected", "[ffmpeg]") {
  SECTION("object detection encodes a downloaded copy, never a device frame") {
    REQUIRE(software_frames_expected(true, 0));
  }

  // The second cause, and the one that caught monitor 36: no object detection,
  // but at the budget transfer_hwframe hands the device frame back and the
  // pipeline continues from the software copy. Shedding cannot be predicted at
  // encoder-open time, so a configured budget is enough to withhold the pool.
  SECTION("a configured device frame budget means shedding can start at any time") {
    REQUIRE(software_frames_expected(false, 8));
  }

  SECTION("no detection and no budget means frames reach the encoder untouched") {
    REQUIRE_FALSE(software_frames_expected(false, 0));
  }

  SECTION("either cause alone is enough") {
    REQUIRE(software_frames_expected(true, 8));
  }
}

TEST_CASE("shed_report_due", "[ffmpeg]") {
  constexpr int64_t kMinute = 60 * 1000 * 1000;

  SECTION("the first shed of a run is always reported") {
    REQUIRE(shed_report_due(12345, 0, kMinute));
  }

  // The defect this exists for: a monitor parked at its cap re-enters shedding
  // several times a second, and every re-entry used to log a Warning, which
  // also writes a row to the Logs table.
  SECTION("re-entering shedding within the interval stays quiet") {
    REQUIRE_FALSE(shed_report_due(kMinute + 400000, kMinute, kMinute));
  }

  SECTION("a continuing episode is reported again once the interval passes") {
    // A whole interval elapsed is due; a microsecond short of it is not.
    REQUIRE(shed_report_due(2 * kMinute, kMinute, kMinute));
    REQUIRE_FALSE(shed_report_due(2 * kMinute - 1, kMinute, kMinute));
  }
}
