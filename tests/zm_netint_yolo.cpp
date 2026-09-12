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

#include "config.h"

#if HAVE_QUADRA

#include "zm_netint_yolo.h"

namespace {

// A frame standing in for a decoded device frame: what makes it usable to the
// hardware AI path is the surface descriptor in data[3], nothing else.
av_frame_ptr DeviceFrame(void *surface) {
  av_frame_ptr frame{av_frame_alloc()};
  frame->data[3] = static_cast<uint8_t *>(surface);
  return frame;
}

av_frame_ptr SoftwareFrame() {
  av_frame_ptr frame{av_frame_alloc()};
  // A software frame carries its planes in data[0..2]; data[3] stays null, and
  // that is precisely what must not reach the scaler.
  frame->data[0] = reinterpret_cast<uint8_t *>(0x1000);
  return frame;
}

// Any non-null value; the helper only tests the pointer, never follows it.
void *const kSurface = reinterpret_cast<void *>(0x2000);

}  // namespace

TEST_CASE("zm_yolo::ai_input_frame software session", "[yolo]") {
  SECTION("runs on the decoded software frame") {
    av_frame_ptr sw = SoftwareFrame();
    REQUIRE(zm_yolo::ai_input_frame(false, nullptr, sw.get()) == sw.get());
  }

  SECTION("prefers the software frame even when a device frame is held") {
    av_frame_ptr hw = DeviceFrame(kSurface);
    av_frame_ptr sw = SoftwareFrame();
    REQUIRE(zm_yolo::ai_input_frame(false, hw.get(), sw.get()) == sw.get());
  }

  SECTION("skips the packet when there is no frame at all") {
    REQUIRE(zm_yolo::ai_input_frame(false, nullptr, nullptr) == nullptr);
  }
}

TEST_CASE("zm_yolo::ai_input_frame hardware session", "[yolo]") {
  SECTION("runs on the device frame") {
    av_frame_ptr hw = DeviceFrame(kSurface);
    av_frame_ptr sw = SoftwareFrame();
    REQUIRE(zm_yolo::ai_input_frame(true, hw.get(), sw.get()) == hw.get());
  }

  // The regression: at the device frame budget transfer_hwframe releases
  // hw_frame and leaves the software copy behind. Falling back to it feeds
  // ni_hwframe_scale a null surface, which dereferences it at offset 8.
  SECTION("skips the packet when the device frame has been shed") {
    av_frame_ptr sw = SoftwareFrame();
    REQUIRE(zm_yolo::ai_input_frame(true, nullptr, sw.get()) == nullptr);
  }

  SECTION("skips the packet when the device frame carries no surface") {
    av_frame_ptr hw = DeviceFrame(nullptr);
    av_frame_ptr sw = SoftwareFrame();
    REQUIRE(zm_yolo::ai_input_frame(true, hw.get(), sw.get()) == nullptr);
  }

  SECTION("skips the packet when nothing was decoded") {
    REQUIRE(zm_yolo::ai_input_frame(true, nullptr, nullptr) == nullptr);
  }
}

#endif  // HAVE_QUADRA
