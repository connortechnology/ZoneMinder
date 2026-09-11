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
#include <vector>

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

/*
 * Budgeting and shedding.
 *
 * These drive the decision function directly rather than through a decode, so
 * the state machine can be pushed across its boundaries deliberately. Each case
 * restores the default budget on the way out; the gauge itself is process-wide,
 * so leaving a budget set would leak into whatever runs next.
 */

namespace {

// Holds frames so occupancy can be placed at a chosen value, and puts the
// budget back however the test leaves.
class BudgetFixture {
 public:
  explicit BudgetFixture(unsigned int budget) : previous_(zm_device_frame_budget()) {
    zm_set_device_frame_budget(budget);
  }
  ~BudgetFixture() {
    held_.clear();
    zm_set_device_frame_budget(previous_);
    // Drain the shedding latch so the next case starts from "not shedding".
    zm_set_device_frame_budget(0);
    zm_device_frame_should_shed();
    zm_set_device_frame_budget(previous_);
  }

  void hold(unsigned int n) {
    for (unsigned int i = 0; i < n; i++) held_.push_back(adopt_device_frame(make_frame()));
  }
  void release(unsigned int n) {
    for (unsigned int i = 0; i < n and !held_.empty(); i++) held_.pop_back();
  }

 private:
  unsigned int previous_;
  std::vector<device_frame_ptr> held_;
};

}  // namespace

TEST_CASE("under budget nothing is shed", "[device_frames]") {
  BudgetFixture f(8);
  f.hold(3);
  REQUIRE_FALSE(zm_device_frame_should_shed());
  REQUIRE_FALSE(zm_device_frames_shedding());
}

TEST_CASE("reaching the budget starts shedding", "[device_frames]") {
  const unsigned int base = zm_device_frames_in_flight();
  BudgetFixture f(base + 4);
  f.hold(4);
  REQUIRE(zm_device_frame_should_shed());
  REQUIRE(zm_device_frames_shedding());
}

TEST_CASE("shedding continues until the low water mark", "[device_frames]") {
  // The point of the hysteresis: dropping one frame puts us back under the
  // budget, so without a low-water mark we would stop shedding immediately and
  // flap across the boundary on every frame.
  BudgetFixture f(0);          // neutralise any inherited occupancy first
  zm_set_device_frame_budget(0);
  zm_device_frame_should_shed();

  const unsigned int base = zm_device_frames_in_flight();
  zm_set_device_frame_budget(base + 8);
  f.hold(8);

  REQUIRE(zm_device_frame_should_shed());   // at budget, start
  f.release(1);
  REQUIRE(zm_device_frame_should_shed());   // one under budget, still shedding
  f.release(2);
  REQUIRE(zm_device_frame_should_shed());   // still above low water
}

TEST_CASE("a budget of zero disables shedding entirely", "[device_frames]") {
  BudgetFixture f(0);
  f.hold(50);
  REQUIRE_FALSE(zm_device_frame_should_shed());
  REQUIRE(zm_device_frame_budget() == 0);
}

TEST_CASE("shed frames are counted", "[device_frames]") {
  const uint64_t before = zm_device_frames_shed();
  const unsigned int base = zm_device_frames_in_flight();
  BudgetFixture f(base + 2);
  f.hold(2);

  REQUIRE(zm_device_frame_should_shed());
  REQUIRE(zm_device_frame_should_shed());
  REQUIRE(zm_device_frames_shed() == before + 2);
}
