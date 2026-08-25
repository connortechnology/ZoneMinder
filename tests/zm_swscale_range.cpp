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
#include "zm_image.h"
#include "zm_swscale.h"

#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// libav emits "deprecated pixel format used, make sure you did set range
// correctly" whenever a context is built with a YUVJ* format. Because swscale
// rewrites that format inside the context, sws_getCachedContext() then never
// matches its cache and rebuilds the context on every single frame - the
// warning appearing once per frame in zmc_*.log is how that shows up. Count
// the warnings to assert we never hand a deprecated format to swscale.
int deprecated_format_warnings = 0;

void counting_log_callback(void *, int, const char *fmt, va_list) {
  if (fmt and strstr(fmt, "deprecated pixel format")) deprecated_format_warnings++;
}

// Installs the counting callback for the lifetime of the test case.
class LogCounter {
 public:
  LogCounter() {
    deprecated_format_warnings = 0;
    av_log_set_callback(counting_log_callback);
  }
  ~LogCounter() { av_log_set_callback(av_log_default_callback); }
  int count() const { return deprecated_format_warnings; }
};

}  // namespace

TEST_CASE("pix_fmt_is_jpeg_range identifies full-range YUVJ formats", "[swscale]") {
  REQUIRE(pix_fmt_is_jpeg_range(AV_PIX_FMT_YUVJ420P));
  REQUIRE(pix_fmt_is_jpeg_range(AV_PIX_FMT_YUVJ422P));
  REQUIRE(pix_fmt_is_jpeg_range(AV_PIX_FMT_YUVJ444P));
  REQUIRE(pix_fmt_is_jpeg_range(AV_PIX_FMT_YUVJ440P));

  REQUIRE_FALSE(pix_fmt_is_jpeg_range(AV_PIX_FMT_YUV420P));
  REQUIRE_FALSE(pix_fmt_is_jpeg_range(AV_PIX_FMT_YUV422P));
  REQUIRE_FALSE(pix_fmt_is_jpeg_range(AV_PIX_FMT_RGB24));
  REQUIRE_FALSE(pix_fmt_is_jpeg_range(AV_PIX_FMT_GRAY8));
}

// Colorimetric regression: a full-range JPEG (YUVJ420P) frame carries luma in
// 0-255. swscale defaults to limited (16-235) input range, which would crush a
// dark grey Y=16 down to ~0 (black). zm_sws_set_input_range() must mark the
// input full range so Y=16 survives as ~16.
TEST_CASE("SWScale treats YUVJ420P input as full range when converting to RGB", "[swscale]") {
  const int w = 16, h = 16;
  const uint8_t Y = 16;  // full range -> ~16; limited range -> ~0

  // YUVJ420P planar: Y plane (w*h), then U and V (w/2*h/2). Neutral chroma=128.
  std::vector<uint8_t> in(SWScale::GetBufferSize(AV_PIX_FMT_YUVJ420P, w, h, 1), 128);
  std::fill(in.begin(), in.begin() + w * h, Y);

  std::vector<uint8_t> out(SWScale::GetBufferSize(AV_PIX_FMT_RGB24, w, h, 1), 0);

  SWScale scaler;
  REQUIRE(scaler.init());
  int r = scaler.Convert(in.data(), in.size(), out.data(), out.size(),
                         AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_RGB24, w, h, 1, 1);
  REQUIRE(r == 0);

  // Full-range interpretation keeps luma ~16; limited-range would crush to ~0.
  REQUIRE(out[0] >= 12);
  REQUIRE(out[0] <= 20);
  // Neutral chroma -> grey, so the three channels stay equal.
  REQUIRE(std::abs(static_cast<int>(out[0]) - static_cast<int>(out[1])) <= 2);
  REQUIRE(std::abs(static_cast<int>(out[1]) - static_cast<int>(out[2])) <= 2);
}

// The mjpeg encode paths (zms, event snapshots) convert INTO YUVJ420P. The
// output end needs the same treatment as the input end: with swscale left on
// its default limited output range, mid grey RGB 128 lands at Y=126 instead of
// Y=128 and the encoded jpeg comes out with a compressed contrast range.
TEST_CASE("SWScale treats YUVJ420P output as full range when converting from RGB", "[swscale]") {
  const int w = 16, h = 16;
  const uint8_t grey = 128;

  std::vector<uint8_t> in(SWScale::GetBufferSize(AV_PIX_FMT_RGB24, w, h, 1), grey);
  std::vector<uint8_t> out(SWScale::GetBufferSize(AV_PIX_FMT_YUVJ420P, w, h, 1), 0);

  SWScale scaler;
  REQUIRE(scaler.init());
  int r = scaler.Convert(in.data(), in.size(), out.data(), out.size(),
                         AV_PIX_FMT_RGB24, AV_PIX_FMT_YUVJ420P, w, h, 1, 1);
  REQUIRE(r == 0);

  // Full range: Y == the RGB level. Limited range would scale it to ~126.
  REQUIRE(out[0] >= 127);
  REQUIRE(out[0] <= 129);
}

TEST_CASE("SWScale does not hand deprecated pixel formats to swscale", "[swscale]") {
  const int w = 16, h = 16;
  LogCounter counter;

  std::vector<uint8_t> yuvj(SWScale::GetBufferSize(AV_PIX_FMT_YUVJ420P, w, h, 1), 128);
  std::vector<uint8_t> rgb(SWScale::GetBufferSize(AV_PIX_FMT_RGB24, w, h, 1), 0);

  SWScale scaler;
  REQUIRE(scaler.init());
  REQUIRE(scaler.Convert(yuvj.data(), yuvj.size(), rgb.data(), rgb.size(),
                         AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_RGB24, w, h, 1, 1) == 0);
  REQUIRE(scaler.Convert(rgb.data(), rgb.size(), yuvj.data(), yuvj.size(),
                         AV_PIX_FMT_RGB24, AV_PIX_FMT_YUVJ420P, w, h, 1, 1) == 0);

  REQUIRE(counter.count() == 0);
}

// Regression: an MJPEG monitor decodes to YUVJ422P and its Image is YUVJ422P
// too, so Image::Assign should take the av_image_copy fast path. Fixing only
// the source format left the identity check comparing YUV422P against
// YUVJ422P: every frame fell through to swscale, which rebuilt its context and
// logged the deprecated-format warning 15+ times a second per monitor.
TEST_CASE("Image::Assign of a matching YUVJ frame copies without swscale", "[swscale]") {
  const int w = 32, h = 16;
  config.font_file_location = "data/fonts/04_valid.zmfnt";

  av_frame_ptr frame{av_frame_alloc()};
  REQUIRE(frame);
  frame->width = w;
  frame->height = h;
  frame->format = AV_PIX_FMT_YUVJ422P;
  REQUIRE(av_frame_get_buffer(frame.get(), 32) == 0);
  // Full-range white: a limited-range conversion would pull this down to 235.
  memset(frame->data[0], 255, frame->linesize[0] * h);
  memset(frame->data[1], 128, frame->linesize[1] * h);
  memset(frame->data[2], 128, frame->linesize[2] * h);

  Image image(frame.get());
  REQUIRE(image.PixFormat() == AV_PIX_FMT_YUVJ422P);

  LogCounter counter;
  REQUIRE(image.Assign(frame.get()));
  REQUIRE(image.Buffer()[0] == 255);
  REQUIRE(counter.count() == 0);
}

TEST_CASE("SWScale does not log deprecated pixel format for YUVJ output", "[swscale]") {
  const int w = 32, h = 32;
  std::vector<uint8_t> in(SWScale::GetBufferSize(AV_PIX_FMT_YUVJ420P, w, h, 1), 128);
  std::vector<uint8_t> out(SWScale::GetBufferSize(AV_PIX_FMT_YUVJ420P, w / 2, h / 2, 1), 0);

  SWScale scaler;
  REQUIRE(scaler.init());

  LogCounter counter;
  int r = scaler.Convert(in.data(), in.size(), out.data(), out.size(),
                         AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUVJ420P,
                         w, h, w / 2, h / 2, 1, 1);
  REQUIRE(r == 0);
  REQUIRE(counter.count() == 0);
}

TEST_CASE("Image::Scale of a YUVJ420P image is silent and range preserving", "[swscale]") {
  const unsigned int w = 64, h = 64;
  const uint8_t Y = 235;  // full range keeps 235; limited-range dst would clip it down

  // Image::Initialise() dereferences config.font_file_location, which is null
  // in the test binary.
  if (!config.font_file_location) config.font_file_location = "";

  Image image(w, h, ZM_COLOUR_GRAY8, ZM_SUBPIX_ORDER_YUVJ420P);
  memset(image.Buffer(), 128, image.Size());
  for (unsigned int y = 0; y < h; y++) {
    memset(image.Buffer() + y * image.LineSize(), Y, w);
  }

  LogCounter counter;
  image.Scale(w / 2, h / 2);
  REQUIRE(counter.count() == 0);

  REQUIRE(image.Width() == w / 2);
  REQUIRE(image.Height() == h / 2);
  REQUIRE(image.PixFormat() == AV_PIX_FMT_YUVJ420P);
  // A full-range -> limited-range conversion would pull 235 down to ~219.
  REQUIRE(image.Buffer()[0] >= 230);
}
