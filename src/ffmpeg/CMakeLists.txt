cmake_minimum_required (VERSION 2.8.7)

include(ConfigureChecks.cmake)
configure_file(config.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config.h @ONLY)

add_library(avengine STATIC ffmpeg.c ffmpeg_filter.c ffmpeg_opt.c cmdutils.c)

