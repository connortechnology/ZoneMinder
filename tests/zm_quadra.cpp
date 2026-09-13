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
  usage.video_mem_usage = 5;
  usage.share_mem_usage = 12;
  usage.p2p_mem_usage = 0;
  usage.total_pixel_load = 148125000;
  return usage;
}

}  // namespace

TEST_CASE("zm_quadra::describe", "[quadra]") {
  SECTION("names the block and the card it is on") {
    const std::string line = zm_quadra::describe(Decoder());
    REQUIRE(line.find("decoder") != std::string::npos);
    REQUIRE(line.find("card 0") != std::string::npos);
  }

  SECTION("reports the instance count") {
    REQUIRE(zm_quadra::describe(Decoder()).find("3 instances") != std::string::npos);
  }

  // The card gives 128 for every block, which is the size of sw_instance[]
  // rather than a limit, so printing it as a denominator would imply headroom
  // nothing has established.
  SECTION("does not present a cap it cannot vouch for") {
    // Specifically that the instance count carries no denominator; the line
    // does contain a slash, in the pixel rate's units.
    REQUIRE(zm_quadra::describe(Decoder()).find("3/") == std::string::npos);
    REQUIRE(zm_quadra::describe(Decoder()).find("128") == std::string::npos);
  }

  // The figures the shared pool does not carry, and the reason for querying the
  // firmware at all: a budget has to be written against what the card holds.
  SECTION("reports the memory the card is actually using") {
    const std::string line = zm_quadra::describe(Decoder());
    REQUIRE(line.find("memory video 5%") != std::string::npos);
    REQUIRE(line.find("share 12%") != std::string::npos);
    REQUIRE(line.find("p2p 0%") != std::string::npos);
  }

  SECTION("reports pixel load") {
    REQUIRE(zm_quadra::describe(Decoder()).find("148.1 Mpixel/s") != std::string::npos);
  }

  SECTION("reports both loads, which disagree often enough to be worth seeing") {
    const std::string line = zm_quadra::describe(Decoder());
    REQUIRE(line.find("load 41%") != std::string::npos);
    REQUIRE(line.find("modelled 38%") != std::string::npos);
  }

  SECTION("prints unknown values rather than a misleading number") {
    zm_quadra::BlockUsage usage = Decoder();
    usage.load = -1;
    usage.model_load = -1;
    usage.video_mem_usage = -1;
    usage.share_mem_usage = -1;
    usage.p2p_mem_usage = -1;
    const std::string line = zm_quadra::describe(usage);
    REQUIRE(line.find("load ?") != std::string::npos);
    // A card that would not answer must not read as one using no memory.
    REQUIRE(line.find("memory video ?") != std::string::npos);
    REQUIRE(line.find("modelled ?") != std::string::npos);
    // A card that reports nothing must not read as a card under no load.
    REQUIRE(line.find("0%") == std::string::npos);
  }

  SECTION("names the block types that are sampled") {
    zm_quadra::BlockUsage usage = Decoder();
    usage.type = NI_DEVICE_TYPE_ENCODER;
    REQUIRE(zm_quadra::describe(usage).find("encoder") != std::string::npos);
  }
}

#endif  // HAVE_QUADRA
