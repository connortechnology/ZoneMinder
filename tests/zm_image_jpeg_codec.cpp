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

// Event writes its frame jpegs through libavcodec rather than libjpeg when it
// can. These check that path produces a real jpeg of the right size, and that
// the quality it is opened with actually reaches the encoder -- the quality
// knob is the part of the old libjpeg path that had to keep working, and
// global_quality only takes effect with AV_CODEC_FLAG_QSCALE set.

#include "zm_catch2.h"
#include "zm_image.h"
#include "zm_utils.h"
#include "zm_swscale.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace {

struct JpegEncoder {
  AVCodecContext *ctx = nullptr;
  SwsContext *sws = nullptr;

  JpegEncoder(int w, int h, int libjpeg_quality) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    REQUIRE(codec != nullptr);
    ctx = avcodec_alloc_context3(codec);
    REQUIRE(ctx != nullptr);
    ctx->width = w;
    ctx->height = h;
    ctx->time_base = AVRational{1, 25};
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->sw_pix_fmt = AV_PIX_FMT_YUVJ420P;
    // Mirrors what Event::OpenJpegCodec does.
    ctx->global_quality = libjpeg_to_ffmpeg_qv(libjpeg_quality) * FF_QP2LAMBDA;
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    REQUIRE(avcodec_open2(ctx, codec, nullptr) >= 0);

    // Same handling the production path uses: hand swscale the non-deprecated
    // format and state the range, rather than letting it warn per context.
    sws = sws_getContext(w, h, AV_PIX_FMT_RGB24,
                         w, h, fix_deprecated_pix_fmt(AV_PIX_FMT_YUVJ420P),
                         SWS_BICUBIC, nullptr, nullptr, nullptr);
    REQUIRE(sws != nullptr);
    zm_sws_set_ranges(sws, AV_PIX_FMT_RGB24, AV_PIX_FMT_YUVJ420P);
  }
  ~JpegEncoder() {
    if (sws) sws_freeContext(sws);
    if (ctx) avcodec_free_context(&ctx);
  }
};

// A gradient rather than a flat colour: a flat image compresses to nearly the
// same size at any quality and would not show the setting having an effect.
// Filled in place because Image is not returnable by value.
void FillGradient(Image &image) {
  const int w = image.Width(), h = image.Height();
  for (int y = 0; y < h; y++) {
    uint8_t *row = image.Buffer(0, y);
    for (int x = 0; x < w; x++) {
      row[x * 3 + 0] = static_cast<uint8_t>((x * 7 + y * 3) & 0xff);
      row[x * 3 + 1] = static_cast<uint8_t>((x * 3 + y * 11) & 0xff);
      row[x * 3 + 2] = static_cast<uint8_t>((x * 13 + y * 5) & 0xff);
    }
  }
}

std::uintmax_t WriteAt(const Image &image, int quality, const std::string &path) {
  JpegEncoder enc(image.Width(), image.Height(), quality);
  REQUIRE(image.WriteJpeg(path, enc.ctx, enc.sws));
  REQUIRE(std::filesystem::exists(path));
  return std::filesystem::file_size(path);
}

bool IsJpeg(const std::string &path) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  unsigned char magic[3] = {0, 0, 0};
  size_t got = fread(magic, 1, 3, f);
  fclose(f);
  // SOI marker, then the start of the first segment.
  return got == 3 and magic[0] == 0xff and magic[1] == 0xd8 and magic[2] == 0xff;
}

}  // namespace

TEST_CASE("Image::WriteJpeg through a codec context", "[image][jpegcodec]") {
  const int w = 320, h = 240;
  Image image(w, h, ZM_COLOUR_RGB24, ZM_SUBPIX_ORDER_RGB);
  FillGradient(image);
  auto dir = std::filesystem::temp_directory_path();

  SECTION("produces a real jpeg") {
    const std::string path = (dir / "zm_jpeg_codec_basic.jpg").string();
    std::filesystem::remove(path);
    const auto size = WriteAt(image, 70, path);
    REQUIRE(size > 0);
    REQUIRE(IsJpeg(path));
    std::filesystem::remove(path);
  }

  SECTION("the quality it is opened with reaches the encoder") {
    // Without AV_CODEC_FLAG_QSCALE, global_quality is ignored and both of these
    // come out the same size. That is the failure this is here to catch.
    const std::string low = (dir / "zm_jpeg_codec_low.jpg").string();
    const std::string high = (dir / "zm_jpeg_codec_high.jpg").string();
    std::filesystem::remove(low);
    std::filesystem::remove(high);

    const auto low_size = WriteAt(image, 20, low);
    const auto high_size = WriteAt(image, 95, high);

    INFO("quality 20 -> " << low_size << " bytes, quality 95 -> " << high_size);
    REQUIRE(IsJpeg(low));
    REQUIRE(IsJpeg(high));
    REQUIRE(high_size > low_size);

    std::filesystem::remove(low);
    std::filesystem::remove(high);
  }
}
