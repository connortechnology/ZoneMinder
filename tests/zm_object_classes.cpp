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

#include <cstdio>
#include <fstream>
#include <string>

#include "zm_object_classes.h"

namespace {

// Write a .names file beside a notional model so loadFromFile() finds it.
std::string WriteNames(const std::string &stem, const std::string &contents) {
  const std::string names = stem + ".names";
  std::ofstream out(names);
  out << contents;
  out.close();
  return stem + ".nb";  // what loadFromFile is handed
}

}  // namespace

TEST_CASE("ObjectClasses::boxColorForName maps the COCO groupings", "[objectclasses]") {
  // These are the groupings the old index-based mapping encoded. Resolving them
  // by name has to give exactly the same answers for COCO.
  SECTION("person is blue") {
    REQUIRE(ObjectClasses::boxColorForName("person") == kRGBBlue);
  }

  SECTION("vehicles are green") {
    for (const char *name : {"bicycle", "car", "motorcycle", "airplane",
                             "bus", "train", "truck", "boat"}) {
      REQUIRE(ObjectClasses::boxColorForName(name) == kRGBGreen);
    }
  }

  SECTION("animals are orange") {
    for (const char *name : {"bird", "cat", "dog", "horse", "sheep",
                             "cow", "elephant", "bear", "zebra", "giraffe"}) {
      REQUIRE(ObjectClasses::boxColorForName(name) == kRGBOrange);
    }
  }

  SECTION("anything else is red") {
    REQUIRE(ObjectClasses::boxColorForName("backpack") == kRGBRed);
    REQUIRE(ObjectClasses::boxColorForName("gun") == kRGBRed);
    REQUIRE(ObjectClasses::boxColorForName("") == kRGBRed);
  }
}

TEST_CASE("ObjectClasses::boxColorFor agrees with the index mapping for COCO",
          "[objectclasses]") {
  // A default-constructed instance carries COCO, so the new name-based lookup
  // must reproduce the legacy index-based one across every class. This is the
  // regression guard: the refactor may not change existing behaviour.
  ObjectClasses coco;
  for (size_t i = 0; i < coco.size(); i++) {
    const int id = static_cast<int>(i);
    REQUIRE(coco.boxColorFor(id) == ObjectClasses::getDetectionBoxColor(id));
    REQUIRE(std::string(coco.colorStringFor(id)) ==
            std::string(ObjectClasses::getDetectionColorString(id)));
  }
}

TEST_CASE("ObjectClasses::boxColorFor does not give a custom model COCO's colours",
          "[objectclasses]") {
  /* The bug this fixes: a firearm model's class 0 is "gun", but the index-based
   * mapping only knows COCO, where 0 is "person" - so a weapon was drawn in the
   * blue reserved for people.
   */
  const std::string model = WriteNames("/tmp/zm_objclasses_firearm", "gun\nrifle\nknife\n");

  ObjectClasses classes;
  REQUIRE(classes.loadFromFile(model));
  REQUIRE(classes.size() == 3);
  REQUIRE(classes.getClassName(0) == "gun");

  // The legacy index lookup still claims blue for class 0 ...
  REQUIRE(ObjectClasses::getDetectionBoxColor(0) == kRGBBlue);
  // ... but resolving through this model's own names does not.
  REQUIRE(classes.boxColorFor(0) == kRGBRed);
  REQUIRE(classes.boxColorFor(1) == kRGBRed);
  REQUIRE(classes.boxColorFor(2) == kRGBRed);
  REQUIRE(std::string(classes.colorStringFor(0)) == "red");

  std::remove("/tmp/zm_objclasses_firearm.names");
}

TEST_CASE("ObjectClasses::boxColorFor still honours names a custom model shares with COCO",
          "[objectclasses]") {
  // A custom model that reorders COCO-ish names must colour by meaning, not by
  // position: "person" is blue wherever it lands.
  const std::string model = WriteNames("/tmp/zm_objclasses_mixed", "gun\nperson\ncar\n");

  ObjectClasses classes;
  REQUIRE(classes.loadFromFile(model));
  REQUIRE(classes.boxColorFor(0) == kRGBRed);     // gun
  REQUIRE(classes.boxColorFor(1) == kRGBBlue);    // person, at index 1 not 0
  REQUIRE(classes.boxColorFor(2) == kRGBGreen);   // car, at index 2 not 2-of-COCO

  std::remove("/tmp/zm_objclasses_mixed.names");
}

TEST_CASE("ObjectClasses::loadFromFile falls back to COCO when no .names exists",
          "[objectclasses]") {
  ObjectClasses classes;
  REQUIRE_FALSE(classes.loadFromFile("/tmp/zm_objclasses_definitely_absent.nb"));
  REQUIRE(classes.size() == 80);
  REQUIRE(classes.getClassName(0) == "person");
  REQUIRE(classes.boxColorFor(0) == kRGBBlue);
}
