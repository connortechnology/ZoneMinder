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

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "zm_netint_lpr.h"

namespace {

// Build a [max_chars][chars_num] recogniser output tensor where each timestep
// is a one-hot over the wanted class index, matching the layout ctc_decode
// consumes.
std::vector<float> BuildTensor(int chars_num, const std::vector<int> &per_timestep) {
  std::vector<float> tensor(per_timestep.size() * chars_num, 0.0f);
  for (size_t n = 0; n < per_timestep.size(); n++) {
    tensor[n * chars_num + per_timestep[n]] = 1.0f;
  }
  return tensor;
}

// A small, obvious charset: index 0 is the CTC blank.
const std::vector<std::string> kTestCharset = {"#", "A", "B", "C", "1", "2"};

std::string TempPath(const std::string &name) {
  return std::string("/tmp/zm_lpr_test_") + name;
}

}  // namespace

TEST_CASE("zm_lpr::utf8_codepoints", "[lpr]") {
  SECTION("splits ASCII one byte per codepoint") {
    REQUIRE(zm_lpr::utf8_codepoints("AB1") == std::vector<std::string>{"A", "B", "1"});
  }

  SECTION("keeps multi-byte codepoints intact") {
    // Three-byte codepoints; splitting by byte would corrupt these.
    std::vector<std::string> got = zm_lpr::utf8_codepoints("京沪A");
    REQUIRE(got.size() == 3);
    REQUIRE(got[0] == "京");
    REQUIRE(got[1] == "沪");
    REQUIRE(got[2] == "A");
  }

  SECTION("empty string yields no codepoints") {
    REQUIRE(zm_lpr::utf8_codepoints("").empty());
  }
}

TEST_CASE("zm_lpr::default_charset", "[lpr]") {
  const std::vector<std::string> &charset = zm_lpr::default_charset();

  SECTION("index 0 is the blank") {
    REQUIRE(charset.at(0) == "#");
  }

  SECTION("carries digits and Latin letters for the plate body") {
    REQUIRE(std::find(charset.begin(), charset.end(), "0") != charset.end());
    REQUIRE(std::find(charset.begin(), charset.end(), "9") != charset.end());
    REQUIRE(std::find(charset.begin(), charset.end(), "A") != charset.end());
    REQUIRE(std::find(charset.begin(), charset.end(), "Z") != charset.end());
  }

  SECTION("omits I and O, which the CCPD charset excludes") {
    REQUIRE(std::find(charset.begin(), charset.end(), "I") == charset.end());
    REQUIRE(std::find(charset.begin(), charset.end(), "O") == charset.end());
  }

  SECTION("every entry is a single codepoint, so indices line up with classes") {
    for (const std::string &entry : charset) {
      REQUIRE(zm_lpr::utf8_codepoints(entry).size() == 1);
    }
  }
}

TEST_CASE("zm_lpr::load_charset", "[lpr]") {
  SECTION("reads one entry per line, preserving order") {
    const std::string path = TempPath("charset.txt");
    {
      std::ofstream out(path);
      out << "_\nA\nB\n7\n";
    }
    std::vector<std::string> charset = zm_lpr::load_charset(path);
    std::remove(path.c_str());

    REQUIRE(charset == std::vector<std::string>{"_", "A", "B", "7"});
  }

  SECTION("tolerates CRLF line endings") {
    const std::string path = TempPath("charset_crlf.txt");
    {
      std::ofstream out(path, std::ios::binary);
      out << "_\r\nA\r\nB\r\n";
    }
    std::vector<std::string> charset = zm_lpr::load_charset(path);
    std::remove(path.c_str());

    REQUIRE(charset == std::vector<std::string>{"_", "A", "B"});
  }

  SECTION("falls back to the built-in charset when the file is missing") {
    std::vector<std::string> charset = zm_lpr::load_charset(TempPath("does_not_exist.txt"));
    REQUIRE(charset == zm_lpr::default_charset());
  }

  SECTION("falls back to the built-in charset when the file is empty") {
    const std::string path = TempPath("charset_empty.txt");
    { std::ofstream out(path); }
    std::vector<std::string> charset = zm_lpr::load_charset(path);
    std::remove(path.c_str());

    REQUIRE(charset == zm_lpr::default_charset());
  }
}

TEST_CASE("zm_lpr::ctc_decode", "[lpr]") {
  const int chars_num = static_cast<int>(kTestCharset.size());

  SECTION("decodes a plain sequence") {
    std::vector<float> tensor = BuildTensor(chars_num, {1, 2, 4});  // A B 1
    REQUIRE(zm_lpr::ctc_decode(tensor.data(), chars_num, 3, kTestCharset) == "AB1");
  }

  SECTION("collapses a run of the same class") {
    std::vector<float> tensor = BuildTensor(chars_num, {1, 1, 1, 2});  // A A A B
    REQUIRE(zm_lpr::ctc_decode(tensor.data(), chars_num, 4, kTestCharset) == "AB");
  }

  SECTION("strips blanks") {
    std::vector<float> tensor = BuildTensor(chars_num, {0, 1, 0, 2, 0});  // _ A _ B _
    REQUIRE(zm_lpr::ctc_decode(tensor.data(), chars_num, 5, kTestCharset) == "AB");
  }

  SECTION("a blank between repeats yields a doubled character") {
    // This is the case NetInt's sample gets wrong: it tracks the last emitted
    // character rather than the last raw one, so it would return "A" here.
    std::vector<float> tensor = BuildTensor(chars_num, {1, 0, 1});  // A _ A
    REQUIRE(zm_lpr::ctc_decode(tensor.data(), chars_num, 3, kTestCharset) == "AA");
  }

  SECTION("a realistic doubled-letter plate survives") {
    // "AA12" as the network would emit it, with blanks separating repeats.
    std::vector<float> tensor =
        BuildTensor(chars_num, {1, 0, 1, 4, 4, 0, 5});  // A _ A 1 1 _ 2
    REQUIRE(zm_lpr::ctc_decode(tensor.data(), chars_num, 7, kTestCharset) == "AA12");
  }

  SECTION("an all-blank output decodes to nothing") {
    std::vector<float> tensor = BuildTensor(chars_num, {0, 0, 0});
    REQUIRE(zm_lpr::ctc_decode(tensor.data(), chars_num, 3, kTestCharset).empty());
  }

  SECTION("takes the argmax when a timestep is not one-hot") {
    std::vector<float> tensor(2 * chars_num, 0.0f);
    tensor[0 * chars_num + 1] = 0.20f;  // A
    tensor[0 * chars_num + 2] = 0.70f;  // B  <- wins
    tensor[1 * chars_num + 4] = 0.90f;  // 1  <- wins
    tensor[1 * chars_num + 5] = 0.10f;  // 2
    REQUIRE(zm_lpr::ctc_decode(tensor.data(), chars_num, 2, kTestCharset) == "B1");
  }

  SECTION("a class index beyond the charset is skipped rather than read out of bounds") {
    std::vector<float> tensor = BuildTensor(chars_num, {1, 2});
    // Hand a charset shorter than chars_num: class 2 has no entry.
    const std::vector<std::string> shortset = {"#", "A"};
    REQUIRE(zm_lpr::ctc_decode(tensor.data(), chars_num, 2, shortset) == "A");
  }
}

TEST_CASE("zm_lpr::order_landmarks", "[lpr]") {
  SECTION("orders a scrambled axis-aligned quad into TL, TR, BL, BR") {
    // Plate spanning (10,20) to (110,60), corners deliberately out of order.
    const double in[4][2] = {
        {110, 60},  // BR
        {10, 20},   // TL
        {10, 60},   // BL
        {110, 20},  // TR
    };
    double out[4][2];
    zm_lpr::order_landmarks(in, out);

    REQUIRE(out[0][0] == Catch::Approx(10));   // TL
    REQUIRE(out[0][1] == Catch::Approx(20));
    REQUIRE(out[1][0] == Catch::Approx(110));  // TR
    REQUIRE(out[1][1] == Catch::Approx(20));
    REQUIRE(out[2][0] == Catch::Approx(10));   // BL
    REQUIRE(out[2][1] == Catch::Approx(60));
    REQUIRE(out[3][0] == Catch::Approx(110));  // BR
    REQUIRE(out[3][1] == Catch::Approx(60));
  }

  SECTION("orders a rotated plate correctly") {
    // A plate tilted clockwise: the left edge sits lower than the right.
    const double in[4][2] = {
        {100, 10},  // TR
        {20, 30},   // TL
        {110, 50},  // BR
        {30, 70},   // BL
    };
    double out[4][2];
    zm_lpr::order_landmarks(in, out);

    REQUIRE(out[0][0] == Catch::Approx(20));   // TL
    REQUIRE(out[0][1] == Catch::Approx(30));
    REQUIRE(out[1][0] == Catch::Approx(100));  // TR
    REQUIRE(out[1][1] == Catch::Approx(10));
    REQUIRE(out[2][0] == Catch::Approx(30));   // BL
    REQUIRE(out[2][1] == Catch::Approx(70));
    REQUIRE(out[3][0] == Catch::Approx(110));  // BR
    REQUIRE(out[3][1] == Catch::Approx(50));
  }

  SECTION("an already-ordered quad is left alone") {
    const double in[4][2] = {{0, 0}, {50, 0}, {0, 25}, {50, 25}};
    double out[4][2];
    zm_lpr::order_landmarks(in, out);

    for (int i = 0; i < 4; i++) {
      REQUIRE(out[i][0] == Catch::Approx(in[i][0]));
      REQUIRE(out[i][1] == Catch::Approx(in[i][1]));
    }
  }
}

TEST_CASE("zm_lpr::perspective_coeffs", "[lpr]") {
  // Map a destination pixel through the coefficients, mirroring how the warp
  // samples: results are source coordinates in 1/256th-pixel fixed point.
  auto map = [](const double f[9], double x, double y, double &u, double &v) {
    const double w = f[6] * x + f[7] * y + f[8];
    u = 256.0 * (f[0] * x + f[1] * y + f[2]) / w;
    v = 256.0 * (f[3] * x + f[4] * y + f[5]) / w;
  };

  SECTION("an axis-aligned source rect gives a pure linear scale") {
    // Source plate rect (30,40)-(230,90), destination 100x25.
    const double landmark[4][2] = {{30, 40}, {230, 40}, {30, 90}, {230, 90}};
    const int out_w = 100;
    const int out_h = 25;
    double f[9];
    zm_lpr::perspective_coeffs(landmark, out_w, out_h, f);

    // No keystoning, so the projective terms vanish.
    REQUIRE(f[6] == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(f[7] == Catch::Approx(0.0).margin(1e-9));

    double u, v;
    map(f, 0, 0, u, v);
    REQUIRE(u / 256.0 == Catch::Approx(30).margin(1e-6));
    REQUIRE(v / 256.0 == Catch::Approx(40).margin(1e-6));

    // Destination (out_w, out_h) is the far corner of the source rect.
    map(f, out_w, out_h, u, v);
    REQUIRE(u / 256.0 == Catch::Approx(230).margin(1e-6));
    REQUIRE(v / 256.0 == Catch::Approx(90).margin(1e-6));

    // Halfway across maps halfway across.
    map(f, out_w / 2.0, out_h / 2.0, u, v);
    REQUIRE(u / 256.0 == Catch::Approx(130).margin(1e-6));
    REQUIRE(v / 256.0 == Catch::Approx(65).margin(1e-6));
  }

  SECTION("the four destination corners land on the four source landmarks") {
    // A keystoned quad, as a plate seen at an angle would produce.
    const double landmark[4][2] = {{50, 30}, {250, 60}, {40, 100}, {260, 150}};
    const int out_w = 94;
    const int out_h = 24;
    double f[9];
    zm_lpr::perspective_coeffs(landmark, out_w, out_h, f);

    struct Corner { double x, y; int idx; };
    const Corner corners[4] = {
        {0, 0, 0},              // TL
        {(double)out_w, 0, 1},  // TR
        {0, (double)out_h, 2},  // BL
        {(double)out_w, (double)out_h, 3},  // BR
    };

    for (const Corner &c : corners) {
      double u, v;
      map(f, c.x, c.y, u, v);
      REQUIRE(u / 256.0 == Catch::Approx(landmark[c.idx][0]).margin(1e-6));
      REQUIRE(v / 256.0 == Catch::Approx(landmark[c.idx][1]).margin(1e-6));
    }
  }

  SECTION("a keystoned quad actually keystones") {
    const double landmark[4][2] = {{50, 30}, {250, 60}, {40, 100}, {260, 150}};
    double f[9];
    zm_lpr::perspective_coeffs(landmark, 94, 24, f);

    // Non-zero projective terms are what distinguish this from an affine warp.
    const bool has_projective =
        std::abs(f[6]) > 1e-9 || std::abs(f[7]) > 1e-9;
    REQUIRE(has_projective);
  }
}
