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

// Image allocates planar YUV buffers with av_image_get_buffer_size(..., 32), so
// where the chroma planes start is ffmpeg's business, not ours. These pin our
// idea of the layout to ffmpeg's by filling a second set of pointers over the
// same buffer with av_image_fill_arrays and requiring they agree.
//
// The distinction that matters is whether the width is a multiple of the
// alignment. Deriving the planes as width*height happens to be right when it
// is, which is why 1920 and 640 wide images never showed a problem.

#include "zm_catch2.h"
#include "zm_image.h"
#include "zm_pixformat.h"
#include "zm_poly.h"
#include "zm_rgb.h"

#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace {

void CheckPlanes(int w, int h) {
  Image image(w, h, ZM_COLOUR_YUV420P, ZM_SUBPIX_ORDER_YUV420P);
  REQUIRE(image.Buffer() != nullptr);

  uint8_t *planes[4] = {};
  int linesizes[4] = {};
  const int filled = av_image_fill_arrays(planes, linesizes, image.Buffer(),
                                          AV_PIX_FMT_YUV420P, w, h, 32);
  REQUIRE(filled > 0);

  INFO("dimensions " << w << "x" << h);
  REQUIRE(image.LineSize() == static_cast<unsigned int>(linesizes[0]));
  REQUIRE(image.UVLineSize() == static_cast<unsigned int>(linesizes[1]));
  REQUIRE(image.UBuffer() == planes[1]);
  REQUIRE(image.VBuffer() == planes[2]);

  // Every plane has to end inside the allocation. The V pointer is the one
  // that used to run off the end: advancing it by a whole Y plane's worth
  // rather than a chroma plane's put it at buffer + size exactly.
  const uint8_t *end = image.Buffer() + image.Size();
  const int chroma_rows = (h + 1) / 2;
  REQUIRE(image.Buffer() + static_cast<size_t>(linesizes[0]) * h <= end);
  REQUIRE(image.UBuffer() + static_cast<size_t>(linesizes[1]) * chroma_rows <= end);
  REQUIRE(image.VBuffer() + static_cast<size_t>(linesizes[2]) * chroma_rows <= end);

  // The planes must not overlap each other either.
  REQUIRE(image.UBuffer() >= image.Buffer() + static_cast<size_t>(linesizes[0]) * h);
  REQUIRE(image.VBuffer() >= image.UBuffer() + static_cast<size_t>(linesizes[1]) * chroma_rows);
}

}  // namespace

TEST_CASE("YUV420P chroma planes follow the ffmpeg layout", "[image][planes]") {
  SECTION("width not a multiple of the alignment") { CheckPlanes(1080, 1920); }
  SECTION("width already aligned")                 { CheckPlanes(1920, 1080); }
  SECTION("odd dimensions")                        { CheckPlanes(641, 481); }
  SECTION("small")                                 { CheckPlanes(640, 480); }
}

TEST_CASE("Writing the last row of each plane stays inside the buffer", "[image][planes]") {
  // A box drawn at the bottom edge touches the last row of every plane, which
  // is where an under-sized plane offset shows up as a write past the end.
  Image image(1080, 1920, ZM_COLOUR_YUV420P, ZM_SUBPIX_ORDER_YUV420P);
  const uint8_t *end = image.Buffer() + image.Size();

  const unsigned int last_y_row = image.Height() - 1;
  REQUIRE(image.Buffer() + static_cast<size_t>(image.LineSize()) * last_y_row
            + image.Width() <= end);

  const unsigned int last_chroma_row = ((image.Height() + 1) / 2) - 1;
  REQUIRE(image.UBuffer() + static_cast<size_t>(image.UVLineSize()) * last_chroma_row
            + (image.Width() + 1) / 2 <= end);
  REQUIRE(image.VBuffer() + static_cast<size_t>(image.UVLineSize()) * last_chroma_row
            + (image.Width() + 1) / 2 <= end);
}

// Outline walks each polygon edge with whichever axis it advances fastest
// along: a mostly-vertical edge steps y and a mostly-horizontal one steps x.
// A rectangle has both, so one call covers the two arms. Both must land the
// converted luma value on the Y plane and the matching chroma on U and V --
// the horizontal arm used to write the raw RGB value into the luma plane and
// touch no chroma at all.
TEST_CASE("Outline draws YUV420P edges on all three planes", "[image][planes]") {
  auto check = [](int w, int h) {
    INFO("dimensions " << w << "x" << h);
    Image image(w, h, ZM_COLOUR_YUV420P, ZM_SUBPIX_ORDER_YUV420P);
    memset(image.Buffer(), 0, image.Size());

    const int left = 64, right = w - 64, top = 64, bottom = h - 64;
    std::vector<Vector2> vertices = {
        Vector2(left, top), Vector2(right, top),
        Vector2(right, bottom), Vector2(left, bottom),
    };
    image.Outline(kRGBRed, Polygon(vertices));

    const YUV expected = brg_to_yuv(kRGBRed);
    const uint8_t y_expected = Y_VAL(expected);
    const uint8_t u_expected = U_VAL(expected);
    const uint8_t v_expected = V_VAL(expected);
    REQUIRE(y_expected != 0);  // otherwise the assertions below prove nothing

    const unsigned int ls = image.LineSize();
    const unsigned int uvls = image.UVLineSize();

    auto luma_at = [&](int x, int y) { return image.Buffer()[y * ls + x]; };
    auto u_at = [&](int x, int y) { return image.UBuffer()[(y / 2) * uvls + (x / 2)]; };
    auto v_at = [&](int x, int y) { return image.VBuffer()[(y / 2) * uvls + (x / 2)]; };

    SECTION("mostly-vertical edge") {
      const int x = left, y = (top + bottom) / 2;
      INFO("left edge at " << x << "," << y);
      REQUIRE(luma_at(x, y) == y_expected);
      REQUIRE(u_at(x, y) == u_expected);
      REQUIRE(v_at(x, y) == v_expected);
    }

    SECTION("mostly-horizontal edge") {
      const int x = (left + right) / 2, y = top;
      INFO("top edge at " << x << "," << y);
      REQUIRE(luma_at(x, y) == y_expected);
      REQUIRE(u_at(x, y) == u_expected);
      REQUIRE(v_at(x, y) == v_expected);
    }

    SECTION("untouched interior stays clear") {
      const int x = (left + right) / 2, y = (top + bottom) / 2;
      REQUIRE(luma_at(x, y) == 0);
    }
  };

  SECTION("width already aligned")            { check(640, 480); }
  SECTION("width not a multiple of the align"){ check(1080, 1920); }
}
