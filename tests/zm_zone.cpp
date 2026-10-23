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

#include "zm_zone.h"

#include "zm_monitor.h"

#include <memory>

TEST_CASE("Zone::ParsePercentagePolygon: full-frame zone at 1920x1080", "[Zone]") {
  Polygon polygon;
  bool result = Zone::ParsePercentagePolygon(
      "0.00,0.00 100.00,0.00 100.00,100.00 0.00,100.00",
      1920, 1080, polygon);

  REQUIRE(result == true);
  REQUIRE(polygon.GetVertices().size() == 4);
  REQUIRE(polygon.GetVertices()[0] == Vector2(0, 0));
  REQUIRE(polygon.GetVertices()[1] == Vector2(1919, 0));
  REQUIRE(polygon.GetVertices()[2] == Vector2(1919, 1079));
  REQUIRE(polygon.GetVertices()[3] == Vector2(0, 1079));
}

TEST_CASE("Zone::ParsePercentagePolygon: center 50% zone", "[Zone]") {
  Polygon polygon;
  bool result = Zone::ParsePercentagePolygon(
      "25.00,25.00 75.00,25.00 75.00,75.00 25.00,75.00",
      1920, 1080, polygon);

  REQUIRE(result == true);
  REQUIRE(polygon.GetVertices().size() == 4);
  REQUIRE(polygon.GetVertices()[0] == Vector2(480, 270));
  REQUIRE(polygon.GetVertices()[1] == Vector2(1440, 270));
  REQUIRE(polygon.GetVertices()[2] == Vector2(1440, 810));
  REQUIRE(polygon.GetVertices()[3] == Vector2(480, 810));
}

TEST_CASE("Zone::ParsePercentagePolygon: fractional percentages", "[Zone]") {
  Polygon polygon;
  bool result = Zone::ParsePercentagePolygon(
      "50.25,75.50 60.00,75.50 60.00,85.00 50.25,85.00",
      1920, 1080, polygon);

  REQUIRE(result == true);
  REQUIRE(polygon.GetVertices().size() == 4);
  // 50.25% of 1920 = 964.8 -> 965
  REQUIRE(polygon.GetVertices()[0].x_ == 965);
  // 75.50% of 1080 = 815.4 -> 815
  REQUIRE(polygon.GetVertices()[0].y_ == 815);
}

TEST_CASE("Zone::ParsePercentagePolygon: different resolution", "[Zone]") {
  Polygon polygon;
  bool result = Zone::ParsePercentagePolygon(
      "0.00,0.00 100.00,0.00 100.00,100.00 0.00,100.00",
      640, 480, polygon);

  REQUIRE(result == true);
  REQUIRE(polygon.GetVertices()[1] == Vector2(639, 0));
  REQUIRE(polygon.GetVertices()[2] == Vector2(639, 479));
}

TEST_CASE("Zone::ParsePercentagePolygon: clamping beyond 100%", "[Zone]") {
  Polygon polygon;
  bool result = Zone::ParsePercentagePolygon(
      "0.00,0.00 110.00,0.00 100.00,100.00 0.00,100.00",
      1920, 1080, polygon);

  REQUIRE(result == true);
  // 110% should be clamped to max valid pixel index (width-1)
  REQUIRE(polygon.GetVertices()[1].x_ == 1919);
}

TEST_CASE("Zone::ParsePercentagePolygon: triangle", "[Zone]") {
  Polygon polygon;
  bool result = Zone::ParsePercentagePolygon(
      "50.00,10.00 90.00,90.00 10.00,90.00",
      1000, 1000, polygon);

  REQUIRE(result == true);
  REQUIRE(polygon.GetVertices().size() == 3);
  REQUIRE(polygon.GetVertices()[0] == Vector2(500, 100));
  REQUIRE(polygon.GetVertices()[1] == Vector2(900, 900));
  REQUIRE(polygon.GetVertices()[2] == Vector2(100, 900));
}

TEST_CASE("Zone::ParsePercentagePolygon: too few points", "[Zone]") {
  Polygon polygon;
  bool result = Zone::ParsePercentagePolygon(
      "0.00,0.00 100.00,0.00",
      1920, 1080, polygon);

  REQUIRE(result == false);
}

TEST_CASE("Zone::ParsePercentagePolygon: integer coords still work", "[Zone]") {
  // strtod handles integers fine
  Polygon polygon;
  bool result = Zone::ParsePercentagePolygon(
      "0,0 100,0 100,100 0,100",
      1920, 1080, polygon);

  REQUIRE(result == true);
  REQUIRE(polygon.GetVertices()[0] == Vector2(0, 0));
  REQUIRE(polygon.GetVertices()[1] == Vector2(1919, 0));
  REQUIRE(polygon.GetVertices()[2] == Vector2(1919, 1079));
  REQUIRE(polygon.GetVertices()[3] == Vector2(0, 1079));
}

TEST_CASE("Zone::ParsePolygonString: basic pixel parsing", "[Zone]") {
  Polygon polygon;

  SECTION("parses a simple rectangle") {
    bool ok = Zone::ParsePolygonString("0,0 639,0 639,479 0,479", polygon);
    REQUIRE(ok);

    auto const &verts = polygon.GetVertices();
    REQUIRE(verts.size() == 4);
    REQUIRE(verts[0] == Vector2(0, 0));
    REQUIRE(verts[1] == Vector2(639, 0));
    REQUIRE(verts[2] == Vector2(639, 479));
    REQUIRE(verts[3] == Vector2(0, 479));
  }

  SECTION("parses a triangle") {
    bool ok = Zone::ParsePolygonString("100,100 200,100 150,200", polygon);
    REQUIRE(ok);

    auto const &verts = polygon.GetVertices();
    REQUIRE(verts.size() == 3);
    REQUIRE(verts[0] == Vector2(100, 100));
    REQUIRE(verts[1] == Vector2(200, 100));
    REQUIRE(verts[2] == Vector2(150, 200));
  }

  SECTION("rejects string with fewer than 3 vertices") {
    // Two vertices — not enough for a polygon, but ParsePolygonString
    // returns true if any vertices were parsed (vertices.empty() check)
    bool ok = Zone::ParsePolygonString("10,20 30,40", polygon);
    REQUIRE(ok);
    // The polygon won't be properly formed but parsing itself doesn't fail
  }
}

TEST_CASE("Zone::ParsePercentagePolygon: percentage to pixel conversion", "[Zone]") {
  Polygon polygon;
  unsigned int width = 1920;
  unsigned int height = 1080;

  SECTION("full-frame 0-100% rectangle") {
    bool ok = Zone::ParsePercentagePolygon("0,0 100,0 100,100 0,100", width, height, polygon);
    REQUIRE(ok);

    auto const &verts = polygon.GetVertices();
    REQUIRE(verts.size() == 4);
    REQUIRE(verts[0] == Vector2(0, 0));
    REQUIRE(verts[1] == Vector2(1919, 0));
    REQUIRE(verts[2] == Vector2(1919, 1079));
    REQUIRE(verts[3] == Vector2(0, 1079));
  }

  SECTION("50% rectangle converts to half-resolution pixels") {
    bool ok = Zone::ParsePercentagePolygon("0,0 50,0 50,50 0,50", width, height, polygon);
    REQUIRE(ok);

    auto const &verts = polygon.GetVertices();
    REQUIRE(verts.size() == 4);
    REQUIRE(verts[0] == Vector2(0, 0));
    REQUIRE(verts[1] == Vector2(960, 0));
    REQUIRE(verts[2] == Vector2(960, 540));
    REQUIRE(verts[3] == Vector2(0, 540));
  }

  SECTION("values are clamped to monitor bounds") {
    // 100% should clamp to max valid pixel index (width-1, height-1)
    bool ok = Zone::ParsePercentagePolygon("0,0 100,0 100,100 0,100", width, height, polygon);
    REQUIRE(ok);

    auto const &verts = polygon.GetVertices();
    for (auto const &v : verts) {
      REQUIRE(v.x_ >= 0);
      REQUIRE(v.x_ < static_cast<int>(width));
      REQUIRE(v.y_ >= 0);
      REQUIRE(v.y_ < static_cast<int>(height));
    }
  }
}

TEST_CASE("Zone: pixel values through ParsePercentagePolygon produce wrong results", "[Zone]") {
  // This test demonstrates the bug: when pixel coordinates (>100) are fed
  // through ParsePercentagePolygon, they get treated as percentages and
  // scaled wildly, then clamped to monitor bounds.
  Polygon polygon;
  unsigned int width = 1920;
  unsigned int height = 1080;

  // These are pixel coordinates for a 640x480 region
  bool ok = Zone::ParsePercentagePolygon("0,0 639,0 639,479 0,479", width, height, polygon);
  REQUIRE(ok);

  auto const &verts = polygon.GetVertices();
  REQUIRE(verts.size() == 4);

  // 639% of 1920 = 12268.8 -> clamped to width-1
  // 479% of 1080 = 5173.2 -> clamped to height-1
  // All non-zero coords get clamped to monitor bounds — the zone is
  // degenerate (covers the full monitor instead of a sub-region)
  REQUIRE(verts[1] == Vector2(static_cast<int>(width) - 1, 0));
  REQUIRE(verts[2] == Vector2(static_cast<int>(width) - 1, static_cast<int>(height) - 1));
}

// --- Auto-detect format tests ---
// The zone loader uses strchr(Coords, '.') to decide format:
//   decimal point present -> ParsePercentagePolygon
//   no decimal point      -> ParsePolygonString (legacy pixels)
// These tests verify both parsers handle the inputs they'll receive
// under auto-detection, and document the broken case that auto-detection prevents.

TEST_CASE("Zone: ParsePolygonString truncates decimal coords via atoi", "[Zone]") {
  // This is the bug that broke motion detection: percentage coords like
  // "0.00,0.00 99.96,0.00 99.96,99.93 0.00,99.93" parsed by
  // ParsePolygonString (which uses atoi) get truncated to a 99x99 pixel zone
  Polygon polygon;
  bool ok = Zone::ParsePolygonString("0.00,0.00 99.96,0.00 99.96,99.93 0.00,99.93", polygon);
  REQUIRE(ok);

  auto const &verts = polygon.GetVertices();
  REQUIRE(verts.size() == 4);
  // atoi("99.96") = 99, atoi("99.93") = 99
  // On a 2560x1440 monitor this would be a 99x99 pixel zone — essentially no coverage
  REQUIRE(verts[1] == Vector2(99, 0));
  REQUIRE(verts[2] == Vector2(99, 99));
}

TEST_CASE("Zone: decimal coords through ParsePercentagePolygon give correct pixels", "[Zone]") {
  // Same coords as above, but correctly routed to ParsePercentagePolygon
  // by the auto-detect logic (decimal point present)
  Polygon polygon;
  unsigned int width = 2560;
  unsigned int height = 1440;

  bool ok = Zone::ParsePercentagePolygon(
      "0.00,0.00 99.96,0.00 99.96,99.93 0.00,99.93", width, height, polygon);
  REQUIRE(ok);

  auto const &verts = polygon.GetVertices();
  REQUIRE(verts.size() == 4);
  // 99.96% of 2560 = 2558.976 -> 2559
  REQUIRE(verts[1].x_ == 2559);
  REQUIRE(verts[1].y_ == 0);
  // 99.93% of 1440 = 1438.992 -> 1439
  REQUIRE(verts[2].x_ == 2559);
  REQUIRE(verts[2].y_ == 1439);
}

TEST_CASE("Zone: integer pixel coords stay as pixels", "[Zone]") {
  // Legacy integer-only coords should be parsed as raw pixel values
  // Auto-detect: no decimal point -> ParsePolygonString
  Polygon polygon;
  bool ok = Zone::ParsePolygonString("0,0 2559,0 2559,1439 0,1439", polygon);
  REQUIRE(ok);

  auto const &verts = polygon.GetVertices();
  REQUIRE(verts.size() == 4);
  REQUIRE(verts[0] == Vector2(0, 0));
  REQUIRE(verts[1] == Vector2(2559, 0));
  REQUIRE(verts[2] == Vector2(2559, 1439));
  REQUIRE(verts[3] == Vector2(0, 1439));
}

TEST_CASE("Zone: auto-detect heuristic — strchr for decimal point", "[Zone]") {
  // Verify the heuristic used by the zone loader:
  // strchr(coords, '.') distinguishes percentage from pixel coords

  // Percentage coords always have decimal points from round(..., 2)
  const char *pct_coords = "0.00,0.00 99.96,0.00 99.96,99.93 0.00,99.93";
  REQUIRE(strchr(pct_coords, '.') != nullptr);

  // Legacy pixel coords are always integers
  const char *px_coords = "0,0 2559,0 2559,1439 0,1439";
  REQUIRE(strchr(px_coords, '.') == nullptr);

  // Edge case: small pixel zone that looks like it could be percentages
  // but has no decimal points — correctly detected as pixels
  const char *small_px = "0,0 50,0 50,50 0,50";
  REQUIRE(strchr(small_px, '.') == nullptr);
}

TEST_CASE("Zone: percentage coords at various resolutions", "[Zone]") {
  Polygon polygon;

  // The same percentage zone should produce proportional pixel coords
  // regardless of monitor resolution
  const char *coords = "10.00,20.00 90.00,20.00 90.00,80.00 10.00,80.00";

  SECTION("640x480") {
    bool ok = Zone::ParsePercentagePolygon(coords, 640, 480, polygon);
    REQUIRE(ok);
    auto const &v = polygon.GetVertices();
    REQUIRE(v[0] == Vector2(64, 96));    // 10% of 640, 20% of 480
    REQUIRE(v[1] == Vector2(576, 96));   // 90% of 640
    REQUIRE(v[2] == Vector2(576, 384));  // 80% of 480
  }

  SECTION("3840x2160 (4K)") {
    bool ok = Zone::ParsePercentagePolygon(coords, 3840, 2160, polygon);
    REQUIRE(ok);
    auto const &v = polygon.GetVertices();
    REQUIRE(v[0] == Vector2(384, 432));   // 10% of 3840, 20% of 2160
    REQUIRE(v[1] == Vector2(3456, 432));  // 90% of 3840
    REQUIRE(v[2] == Vector2(3456, 1728)); // 80% of 2160
  }
}

// Pixel coordinates are used unscaled, so a zone drawn on a larger monitor
// lies partly outside a smaller one. Zone::Setup rasterises the polygon into a
// monitor-sized image, which overflowed the heap before clamping.
TEST_CASE("Zone::ClampPolygon: pixel zone larger than the monitor", "[Zone]") {
  Polygon polygon;
  REQUIRE(Zone::ParsePolygonString("0,0 1279,0 1279,719 0,719", polygon));

  REQUIRE(Zone::ClampPolygon(polygon, 640, 360) == true);
  REQUIRE(polygon.GetVertices().size() == 4);
  REQUIRE(polygon.GetVertices()[0] == Vector2(0, 0));
  REQUIRE(polygon.GetVertices()[1] == Vector2(639, 0));
  REQUIRE(polygon.GetVertices()[2] == Vector2(639, 359));
  REQUIRE(polygon.GetVertices()[3] == Vector2(0, 359));
  REQUIRE(polygon.Extent().Hi() == Vector2(639, 359));
}

TEST_CASE("Zone::ClampPolygon: negative pixel coordinates", "[Zone]") {
  Polygon polygon;
  REQUIRE(Zone::ParsePolygonString("-10,-5 100,-5 100,50", polygon));

  REQUIRE(Zone::ClampPolygon(polygon, 640, 360) == true);
  REQUIRE(polygon.GetVertices()[0] == Vector2(0, 0));
  REQUIRE(polygon.GetVertices()[1] == Vector2(100, 0));
  REQUIRE(polygon.GetVertices()[2] == Vector2(100, 50));
  REQUIRE(polygon.Extent().Lo() == Vector2(0, 0));
}

TEST_CASE("Zone::ClampPolygon: zone inside the monitor is unchanged", "[Zone]") {
  Polygon polygon;
  REQUIRE(Zone::ParsePolygonString("0,0 639,0 639,359 0,359", polygon));

  REQUIRE(Zone::ClampPolygon(polygon, 640, 360) == false);
  REQUIRE(polygon.GetVertices()[2] == Vector2(639, 359));
}

namespace {

// The Monitor constructor strcmp()s config.event_close_mode and Image reads
// config.font_file_location; both are null without a database.
void EnsureMotionConfig() {
  if (!config.font_file_location) config.font_file_location = "";
  if (!config.event_close_mode) config.event_close_mode = "idle";
}

// A capture size and a motion frame width, without a database.
class MotionMonitor : public Monitor {
 public:
  MotionMonitor(unsigned int w, unsigned int h, unsigned int motion_w) : Monitor() {
    width = w;
    height = h;
    colours = ZM_COLOUR_GRAY8;
    motion_frame_width = motion_w;
    // >1 is what has CheckAlarms build the alarm highlight.
    savejpegs = 2;
  }
};

std::shared_ptr<Monitor> MakeMotionMonitor(unsigned int w, unsigned int h, unsigned int motion_w) {
  return std::static_pointer_cast<Monitor>(std::make_shared<MotionMonitor>(w, h, motion_w));
}

Polygon Rect(int lo_x, int lo_y, int hi_x, int hi_y) {
  return Polygon({Vector2(lo_x, lo_y), Vector2(hi_x, lo_y), Vector2(hi_x, hi_y), Vector2(lo_x, hi_y)});
}

}  // namespace

// Zones stored in pixels were drawn against the capture. On a monitor that
// detects motion on a smaller frame, their corners move to the same place in
// that frame.
TEST_CASE("Zone::ScalePolygon maps capture pixels onto the motion frame", "[Zone]") {
  const Polygon scaled = Zone::ScalePolygon(Rect(0, 0, 3839, 2159), 3840, 2160, 1280, 720);
  const std::vector<Vector2> &v = scaled.GetVertices();
  REQUIRE(v.size() == 4);
  REQUIRE(v[0] == Vector2(0, 0));
  REQUIRE(v[1] == Vector2(1279, 0));
  REQUIRE(v[2] == Vector2(1279, 719));
  REQUIRE(v[3] == Vector2(0, 719));

  const Polygon middle = Zone::ScalePolygon(Rect(1920, 1080, 2879, 1619), 3840, 2160, 1280, 720);
  REQUIRE(middle.GetVertices()[0] == Vector2(640, 360));
  REQUIRE(middle.GetVertices()[2] == Vector2(959, 539));
}

// Pixel thresholds scale with area and the filter box with length. A threshold
// that was set stays set: rounding it down to 0 would turn it off.
TEST_CASE("Zone thresholds scale with the motion frame", "[Zone]") {
  SECTION("pixel counts scale by area") {
    REQUIRE(Zone::ScalePixelCount(900, 1.0 / 9) == 100);
    REQUIRE(Zone::ScalePixelCount(0, 1.0 / 9) == 0);
    REQUIRE(Zone::ScalePixelCount(5, 1.0 / 9) == 1);
  }

  SECTION("lengths scale by width") {
    REQUIRE(Zone::ScaleLength(9, 1.0 / 3) == 3);
    REQUIRE(Zone::ScaleLength(0, 1.0 / 3) == 0);
    REQUIRE(Zone::ScaleLength(3, 1.0 / 3) == 1);
    REQUIRE(Zone::ScaleLength(2, 1.0 / 3) == 1);
  }
}

TEST_CASE("Zones are checked at the monitor's motion frame size", "[Zone]") {
  EnsureMotionConfig();
  auto monitor = MakeMotionMonitor(3840, 2160, 1280);
  REQUIRE(monitor->MotionWidth() == 1280);
  REQUIRE(monitor->MotionHeight() == 720);

  SECTION("an active zone's mask is built at the motion size") {
    Zone zone(monitor, 1, "yard", Zone::ACTIVE, Rect(0, 0, 1279, 719), kRGBRed, Zone::ALARMED_PIXELS);
    REQUIRE(zone.getPgImage()->Width() == 1280);
    REQUIRE(zone.getPgImage()->Height() == 720);
  }

  SECTION("a privacy zone stays at the capture size") {
    Zone zone(monitor, 2, "privacy", Zone::PRIVACY, Rect(0, 0, 3839, 2159));
    REQUIRE(zone.getPgImage()->Width() == 3840);
    REQUIRE(zone.getPgImage()->Height() == 2160);
  }
}

// Stats rows, PTZ tracking and the analysis image all work in capture
// coordinates, so what a zone reports comes back to that size however small a
// frame it checked.
TEST_CASE("Zone stats are reported at the capture size", "[Zone]") {
  EnsureMotionConfig();
  auto monitor = MakeMotionMonitor(3840, 2160, 1280);

  Image delta(1280, 720, ZM_COLOUR_GRAY8, ZM_SUBPIX_ORDER_NONE);
  for (unsigned int y = 0; y < delta.Height(); y++)
    memset(delta.Buffer(0, y), 255, delta.Width());

  SECTION("pixel counts") {
    Zone zone(monitor, 1, "yard", Zone::ACTIVE, Rect(0, 0, 1279, 719), kRGBRed, Zone::ALARMED_PIXELS,
              /*min_pixel_threshold*/ 10, /*max_pixel_threshold*/ 0,
              /*min_alarm_pixels*/ 1, /*max_alarm_pixels*/ 0);
    REQUIRE(zone.CheckAlarms(&delta));
    REQUIRE(zone.GetStats().alarm_pixels_ == 3840u * 2160u);

    // The highlight is laid over the full size analysis image.
    REQUIRE(zone.AlarmImage() != nullptr);
    REQUIRE(zone.AlarmImage()->Width() == 3840);
    REQUIRE(zone.AlarmImage()->Height() == 2160);
  }

  // Only the blob check works out where in the zone the alarm is.
  SECTION("alarm box and centre") {
    Zone zone(monitor, 1, "yard", Zone::ACTIVE, Rect(0, 0, 1279, 719), kRGBRed, Zone::BLOBS,
              /*min_pixel_threshold*/ 10, /*max_pixel_threshold*/ 0,
              /*min_alarm_pixels*/ 1, /*max_alarm_pixels*/ 0, Vector2(3, 3),
              /*min_filter_pixels*/ 1, /*max_filter_pixels*/ 0,
              /*min_blob_pixels*/ 1, /*max_blob_pixels*/ 0);
    REQUIRE(zone.CheckAlarms(&delta));
    const ZoneStats &stats = zone.GetStats();
    REQUIRE(stats.alarm_blobs_ == 1);
    REQUIRE(stats.alarm_box_.Lo() == Vector2(0, 0));
    REQUIRE(stats.alarm_box_.Hi() == Vector2(3839, 2159));
    REQUIRE(stats.alarm_centre_.x_ >= 1918);
    REQUIRE(stats.alarm_centre_.x_ <= 1921);
    REQUIRE(stats.alarm_centre_.y_ >= 1078);
    REQUIRE(stats.alarm_centre_.y_ <= 1081);
  }
}
