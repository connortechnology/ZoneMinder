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

#include "zm_quadra.h"

namespace {

zm_quadra::BlockUsage Decoder() {
  zm_quadra::BlockUsage usage;
  usage.type = NI_DEVICE_TYPE_DECODER;
  usage.card_idx = 0;
  usage.load = 41;
  usage.model_load = 38;
  usage.active_instances = 3;
  usage.max_instances = 32;
  usage.active_pixels = 3ULL * 3840 * 2160;
  return usage;
}

}  // namespace

TEST_CASE("zm_quadra::describe", "[quadra]") {
  SECTION("names the block and the card it is on") {
    const std::string line = zm_quadra::describe(Decoder());
    REQUIRE(line.find("decoder") != std::string::npos);
    REQUIRE(line.find("card 0") != std::string::npos);
  }

  SECTION("reports instances against the cap") {
    REQUIRE(zm_quadra::describe(Decoder()).find("3/32 instances") != std::string::npos);
  }

  SECTION("reports both loads, which disagree often enough to be worth seeing") {
    const std::string line = zm_quadra::describe(Decoder());
    REQUIRE(line.find("load 41%") != std::string::npos);
    REQUIRE(line.find("modelled 38%") != std::string::npos);
  }

  SECTION("reports active pixels, the quantity card memory is actually spent on") {
    // 3 * 3840 * 2160 = 24883200 pixels.
    REQUIRE(zm_quadra::describe(Decoder()).find("24.9 Mpixel") != std::string::npos);
  }

  SECTION("prints unknown values rather than a misleading number") {
    zm_quadra::BlockUsage usage = Decoder();
    usage.load = -1;
    usage.model_load = -1;
    usage.max_instances = -1;
    const std::string line = zm_quadra::describe(usage);
    REQUIRE(line.find("load ?") != std::string::npos);
    REQUIRE(line.find("modelled ?") != std::string::npos);
    REQUIRE(line.find("3/? instances") != std::string::npos);
    // A card that reports nothing must not read as a card under no load.
    REQUIRE(line.find("0%") == std::string::npos);
  }

  SECTION("names every block type") {
    zm_quadra::BlockUsage usage = Decoder();
    usage.type = NI_DEVICE_TYPE_ENCODER;
    REQUIRE(zm_quadra::describe(usage).find("encoder") != std::string::npos);
    usage.type = NI_DEVICE_TYPE_SCALER;
    REQUIRE(zm_quadra::describe(usage).find("scaler") != std::string::npos);
    usage.type = NI_DEVICE_TYPE_AI;
    REQUIRE(zm_quadra::describe(usage).find("ai") != std::string::npos);
  }
}

#endif  // HAVE_QUADRA
