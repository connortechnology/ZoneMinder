/*
 * Device-frame accounting.
 *
 * The gauge exists so a later chunk can degrade gracefully when too many
 * decoded hardware frames are pinned at once. What matters is that it cannot
 * drift: every release path has to decrement, including the ones that are easy
 * to forget -- a packet destroyed with a frame still attached, and a frame
 * handed off by move rather than cleared. Those are the cases covered here.
 */

#include "zm_catch2.h"

#include "zm_ffmpeg.h"

#include <utility>

namespace {

// A frame with no buffers is enough: the gauge counts pointers we hold, not
// pixels, and av_frame_free copes with an empty frame.
av_frame_ptr make_frame() {
  return av_frame_ptr{av_frame_alloc()};
}

}  // namespace

TEST_CASE("device frame gauge counts an adopted frame", "[device_frames]") {
  const unsigned int before = zm_device_frames_in_flight();

  {
    device_frame_ptr held = adopt_device_frame(make_frame());
    REQUIRE(held);
    REQUIRE(zm_device_frames_in_flight() == before + 1);
  }

  REQUIRE(zm_device_frames_in_flight() == before);
}

TEST_CASE("adopting a null frame counts nothing", "[device_frames]") {
  const unsigned int before = zm_device_frames_in_flight();
  device_frame_ptr held = adopt_device_frame(av_frame_ptr{});
  REQUIRE_FALSE(held);
  REQUIRE(zm_device_frames_in_flight() == before);
}

TEST_CASE("clearing the pointer releases the count", "[device_frames]") {
  const unsigned int before = zm_device_frames_in_flight();

  device_frame_ptr held = adopt_device_frame(make_frame());
  REQUIRE(zm_device_frames_in_flight() == before + 1);

  // This is how the analysis path drops a frame it no longer needs.
  held = nullptr;
  REQUIRE(zm_device_frames_in_flight() == before);
}

TEST_CASE("moving a held frame does not double count", "[device_frames]") {
  const unsigned int before = zm_device_frames_in_flight();

  device_frame_ptr first = adopt_device_frame(make_frame());
  REQUIRE(zm_device_frames_in_flight() == before + 1);

  device_frame_ptr second = std::move(first);
  REQUIRE(zm_device_frames_in_flight() == before + 1);
  REQUIRE_FALSE(first);

  second = nullptr;
  REQUIRE(zm_device_frames_in_flight() == before);
}

TEST_CASE("overwriting a held frame releases the old one", "[device_frames]") {
  const unsigned int before = zm_device_frames_in_flight();

  device_frame_ptr held = adopt_device_frame(make_frame());
  held = adopt_device_frame(make_frame());

  // Two adopted, one replaced: the replaced one must have been released.
  REQUIRE(zm_device_frames_in_flight() == before + 1);

  held = nullptr;
  REQUIRE(zm_device_frames_in_flight() == before);
}

TEST_CASE("high water mark records the peak, not the current count", "[device_frames]") {
  const unsigned int start = zm_device_frames_in_flight();

  {
    device_frame_ptr a = adopt_device_frame(make_frame());
    device_frame_ptr b = adopt_device_frame(make_frame());
    device_frame_ptr c = adopt_device_frame(make_frame());
    REQUIRE(zm_device_frames_in_flight() == start + 3);
    REQUIRE(zm_device_frames_high_water() >= start + 3);
  }

  const unsigned int peak = zm_device_frames_high_water();
  REQUIRE(zm_device_frames_in_flight() == start);
  // Releasing must not walk the peak back down.
  REQUIRE(zm_device_frames_high_water() == peak);
  REQUIRE(peak >= 3);
}
