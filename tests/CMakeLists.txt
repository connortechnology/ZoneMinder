# This file is part of the ZoneMinder Project.
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.


include(Catch)

set(TEST_SOURCES
  zm_box.cpp
  zm_comms.cpp
  zm_crypt.cpp
  zm_font.cpp
  zm_onvif_renewal.cpp
  zm_pixformat.cpp
  zm_poly.cpp
  zm_utils.cpp
  zm_vector2.cpp
  zm_yolov8_onnx_postprocess.cpp
  zm_zone.cpp)

add_executable(tests main.cpp ${TEST_SOURCES})

target_link_libraries(tests
  PRIVATE
    zm-core-interface
    zm
    ${ZM_BIN_LIBS}
    bcrypt
    Catch2::Catch2WithMain)

target_include_directories(tests
  PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR})

# Match the production binaries: bake the OpenVINO runtime dir into RPATH so
# the test executable can be run (and discovered by Catch) without sourcing
# setupvars.sh or polluting LD_LIBRARY_PATH. DL_PATHS is also supplied as a
# safety net for the discovery step, which runs the binary right after linking.
if(OPENVINO_LIB_DIR)
  target_link_options(tests PRIVATE "-Wl,-rpath,${OPENVINO_LIB_DIR}")
  catch_discover_tests(tests DL_PATHS "${OPENVINO_LIB_DIR}")
else()
  catch_discover_tests(tests)
endif()

add_custom_command(TARGET tests
  PRE_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ ${CMAKE_CURRENT_BINARY_DIR}/data/
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/data/)
