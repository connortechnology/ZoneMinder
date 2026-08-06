//
// ZoneMinder Object Classes Header
// Copyright (C) 2024 ZoneMinder Inc
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#ifndef ZM_OBJECT_CLASSES_H
#define ZM_OBJECT_CLASSES_H

#include "zm_rgb.h"

#include <string>
#include <vector>

// Manages object class names for detection models.
// Provides default COCO classes and supports loading custom classes from .names files.
class ObjectClasses {
 public:
  // Default constructor uses COCO class names
  ObjectClasses();

  // Load class names from a .names file associated with the model file.
  // If no .names file is found, falls back to COCO defaults.
  // Returns true if custom classes were loaded, false if using defaults.
  bool loadFromFile(const std::string &model_file);

  // Get class name by ID with bounds checking.
  // Returns "unknown" for out-of-range IDs.
  const std::string& getClassName(int class_id) const;

  // Get number of classes
  size_t size() const { return class_names_.size(); }

  // Get detection box color based on class ID.
  // Person (class 0) = Blue, Vehicles (1-8) = Green, Animals (14-23) = Orange, Others = Red
  //
  // These take a raw index and can therefore only assume the COCO ordering. A
  // custom model's class 0 is not "person", so prefer the instance methods
  // below, which resolve the index through this object's own class list.
  static Rgb getDetectionBoxColor(int class_id);

  // Get detection color name as string.
  static const char* getDetectionColorString(int class_id);

  /* Colour for a detection box, chosen from the class *name* rather than its
   * index. An index only means anything within the dataset it came from, so a
   * firearm model whose class 0 is "gun" would otherwise be drawn in COCO's
   * "person" blue. Resolving by name gives identical results for COCO and
   * something sensible - red, the default - for classes we have no opinion on.
   */
  Rgb boxColorFor(int class_id) const;
  const char *colorStringFor(int class_id) const;

  // Name-based colour lookup, exposed so it can be tested directly.
  static Rgb boxColorForName(const std::string &class_name);
  static const char *colorStringForName(const std::string &class_name);

  // Access to underlying vector for iteration
  const std::vector<std::string>& getClassNames() const { return class_names_; }

 private:
  std::vector<std::string> class_names_;

  // Default COCO class names (80 classes)
  static const std::vector<std::string> kCocoClassNames;
  static const std::string kUnknownClass;
};

#endif  // ZM_OBJECT_CLASSES_H
