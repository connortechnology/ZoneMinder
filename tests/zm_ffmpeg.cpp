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

namespace {

// encoder_share_pool only ever passes the pointer through or drops it, so a
// stand-in is enough; nothing dereferences it.
AVBufferRef *const kDecoderPool = reinterpret_cast<AVBufferRef *>(0x1000);

}  // namespace

TEST_CASE("encoder_share_pool", "[ffmpeg]") {
  SECTION("shares the decoder pool when frames go straight to the encoder") {
    REQUIRE(encoder_share_pool(kDecoderPool, false) == kDecoderPool);
  }

  // The regression: object detection encodes a software copy with boxes drawn
  // on it, so every frame needs av_hwframe_get_buffer(), which fails with
  // EINVAL against a pool the decoder owns and ends recording for the event.
  SECTION("withholds the pool when frames are rewritten in software") {
    REQUIRE(encoder_share_pool(kDecoderPool, true) == nullptr);
  }

  SECTION("has nothing to share when the decoder has no pool") {
    REQUIRE(encoder_share_pool(nullptr, false) == nullptr);
    REQUIRE(encoder_share_pool(nullptr, true) == nullptr);
  }
}

TEST_CASE("software_frames_expected", "[ffmpeg]") {
  SECTION("object detection encodes a downloaded copy, never a device frame") {
    REQUIRE(software_frames_expected(true, 0));
  }

  // The second cause, and the one that caught monitor 36: no object detection,
  // but at the budget transfer_hwframe hands the device frame back and the
  // pipeline continues from the software copy. Shedding cannot be predicted at
  // encoder-open time, so a configured budget is enough to withhold the pool.
  SECTION("a configured device frame budget means shedding can start at any time") {
    REQUIRE(software_frames_expected(false, 8));
  }

  SECTION("no detection and no budget means frames reach the encoder untouched") {
    REQUIRE_FALSE(software_frames_expected(false, 0));
  }

  SECTION("either cause alone is enough") {
    REQUIRE(software_frames_expected(true, 8));
  }
}

TEST_CASE("shed_report_due", "[ffmpeg]") {
  constexpr int64_t kMinute = 60 * 1000 * 1000;

  SECTION("the first shed of a run is always reported") {
    REQUIRE(shed_report_due(12345, 0, kMinute));
  }

  // The defect this exists for: a monitor parked at its cap re-enters shedding
  // several times a second, and every re-entry used to log a Warning, which
  // also writes a row to the Logs table.
  SECTION("re-entering shedding within the interval stays quiet") {
    REQUIRE_FALSE(shed_report_due(kMinute + 400000, kMinute, kMinute));
  }

  SECTION("a continuing episode is reported again once the interval passes") {
    // A whole interval elapsed is due; a microsecond short of it is not.
    REQUIRE(shed_report_due(2 * kMinute, kMinute, kMinute));
    REQUIRE_FALSE(shed_report_due(2 * kMinute - 1, kMinute, kMinute));
  }
}

TEST_CASE("effective_device_frame_budget", "[ffmpeg]") {
  constexpr unsigned int kGlobal = 8;

  SECTION("one monitor with no override takes the global budget") {
    REQUIRE(effective_device_frame_budget({-1}, kGlobal) == kGlobal);
  }

  SECTION("one monitor with an override takes its own") {
    REQUIRE(effective_device_frame_budget({16}, kGlobal) == 16);
  }

  // The distinction the web form has to preserve: a blank field means "no
  // override" and stores NULL, which arrives here as -1. Zero is a real
  // setting meaning no cap at all, and must not be confused with it.
  SECTION("zero is no cap, not the default") {
    REQUIRE(effective_device_frame_budget({0}, kGlobal) == 0);
    REQUIRE(effective_device_frame_budget({-1}, kGlobal) != 0);
  }

  // Budgets are deliberately not added. Servers commonly hold more than one
  // card, the gauge has no card dimension, and which card a monitor is on is
  // not known until it decodes -- long after this is resolved. Adding them
  // would let the monitors on one card pin the combined figure on that card.
  SECTION("a daemon serving several monitors takes the smallest, not the sum") {
    REQUIRE(effective_device_frame_budget({16, 4}, kGlobal) == 4);
    REQUIRE(effective_device_frame_budget({4, 16}, kGlobal) == 4);
    REQUIRE(effective_device_frame_budget({-1, -1}, kGlobal) == kGlobal);
    REQUIRE(effective_device_frame_budget({-1, 4}, kGlobal) == 4);
  }

  SECTION("one monitor asking for no cap uncaps the process") {
    REQUIRE(effective_device_frame_budget({16, 0}, kGlobal) == 0);
  }

  SECTION("no monitors falls back to the global budget") {
    REQUIRE(effective_device_frame_budget({}, kGlobal) == kGlobal);
  }
}

TEST_CASE("hw_frame_bytes", "[ffmpeg]") {
  SECTION("yuv420p costs width * height * 1.5") {
    REQUIRE(hw_frame_bytes(AV_PIX_FMT_YUV420P, 1920, 1080) == 1920 * 1080 * 3 / 2);
    REQUIRE(hw_frame_bytes(AV_PIX_FMT_YUV420P, 640, 480) == 640 * 480 * 3 / 2);
  }

  // The point of measuring in bytes: a frame count means wildly different
  // things depending on the monitor, so a budget written in frames cannot be
  // compared across them.
  SECTION("4K costs about nine times what 720p does") {
    const int64_t uhd = hw_frame_bytes(AV_PIX_FMT_YUV420P, 3840, 2160);
    const int64_t hd = hw_frame_bytes(AV_PIX_FMT_YUV420P, 1280, 720);
    REQUIRE(uhd == 9 * hd);
  }

  // Pins the packed size specifically. An allocator-aligned figure would be
  // larger here and would vary with whatever alignment happened to be passed,
  // which is no use for comparing one monitor against another.
  SECTION("a width that is not a multiple of the alignment is still packed") {
    REQUIRE(hw_frame_bytes(AV_PIX_FMT_YUV420P, 700, 500) == 700 * 500 * 3 / 2);
  }

  SECTION("nv12 costs the same as yuv420p, being the same sampling") {
    REQUIRE(hw_frame_bytes(AV_PIX_FMT_NV12, 1920, 1080) ==
            hw_frame_bytes(AV_PIX_FMT_YUV420P, 1920, 1080));
  }

  SECTION("an unusable format or geometry costs nothing rather than guessing") {
    REQUIRE(hw_frame_bytes(AV_PIX_FMT_NONE, 1920, 1080) == 0);
    REQUIRE(hw_frame_bytes(AV_PIX_FMT_YUV420P, 0, 1080) == 0);
    REQUIRE(hw_frame_bytes(AV_PIX_FMT_YUV420P, 1920, -1) == 0);
  }
}

TEST_CASE("describe_hw_pool_line", "[ffmpeg]") {
  HwPoolInfo info;
  info.pool_size = 20;
  info.width = 1920;
  info.height = 1080;
  info.sw_format = AV_PIX_FMT_YUV420P;
  info.frame_bytes = hw_frame_bytes(info.sw_format, info.width, info.height);
  info.pool_bytes = info.frame_bytes * info.pool_size;

  SECTION("names the format, the slots and the geometry") {
    const std::string line = describe_hw_pool_line(info);
    REQUIRE(line.find("yuv420p") != std::string::npos);
    REQUIRE(line.find("20 frames") != std::string::npos);
    REQUIRE(line.find("1920x1080") != std::string::npos);
  }

  SECTION("reports the reservation, which is what card memory actually goes on") {
    // 20 * 1920 * 1080 * 1.5 = 59.3 MB
    REQUIRE(describe_hw_pool_line(info).find("59.3 MB reserved") != std::string::npos);
  }

  // A pool that grows on demand has no fixed ceiling, which is a different
  // thing from a pool with no slots and must not read as one.
  SECTION("a growing pool says so rather than reading as empty") {
    info.pool_size = 0;
    info.pool_bytes = 0;
    const std::string line = describe_hw_pool_line(info);
    REQUIRE(line.find("on-demand frames") != std::string::npos);
    REQUIRE(line.find("0 frames") == std::string::npos);
  }
}

TEST_CASE("av_log_should_print", "[ffmpeg]") {
  constexpr int64_t kMinute = 60 * 1000 * 1000;
  AvLogRepeat state;
  uint64_t suppressed = 0;

  SECTION("the first message of its kind prints") {
    REQUIRE(av_log_should_print(state, "SEI type 764 truncated", 1000, kMinute, &suppressed));
    REQUIRE(suppressed == 0);
  }

  // The case that filled 40MB of one monitor's log: libavcodec reports this
  // per frame at AV_LOG_ERROR, which ZM maps to Warning, which is a database
  // row each time.
  SECTION("repeats inside the interval are counted, not printed") {
    REQUIRE(av_log_should_print(state, "SEI truncated", 0, kMinute, &suppressed));
    for (int i = 1; i <= 500; i++) {
      REQUIRE_FALSE(av_log_should_print(state, "SEI truncated", i * 1000, kMinute, &suppressed));
    }
    REQUIRE(av_log_should_print(state, "SEI truncated", kMinute, kMinute, &suppressed));
    REQUIRE(suppressed == 500);
  }

  SECTION("a different message prints immediately rather than waiting") {
    REQUIRE(av_log_should_print(state, "first", 0, kMinute, &suppressed));
    REQUIRE(av_log_should_print(state, "second", 1000, kMinute, &suppressed));
  }

  // The failure the first version shipped with. The camera's SEI complaints
  // cycle through three sizes, so every message differs from the one before,
  // and a scheme that only remembers the previous message suppresses nothing.
  SECTION("messages that alternate are each limited on their own") {
    const char *distinct[] = {"size 66", "size 50", "size 34"};
    for (const char *m : distinct) REQUIRE(av_log_should_print(state, m, 0, kMinute, &suppressed));

    // Now the real traffic, which repeats some of them within a round.
    const char *cycle[] = {"size 66", "size 66", "size 50", "size 50", "size 34"};
    // Still inside the interval: every one of them is now known.
    for (int round = 1; round < 200; round++) {
      for (const char *m : cycle) {
        REQUIRE_FALSE(av_log_should_print(state, m, round * 1000, kMinute, &suppressed));
      }
    }

    // Each reports its own count: 199 rounds, and "size 66" appears twice a round.
    REQUIRE(av_log_should_print(state, "size 66", kMinute, kMinute, &suppressed));
    REQUIRE(suppressed == 2 * 199);
    REQUIRE(av_log_should_print(state, "size 34", kMinute, kMinute, &suppressed));
    REQUIRE(suppressed == 199);
  }

  SECTION("an endlessly varying source costs a bounded amount") {
    for (int i = 0; i < 10 * static_cast<int>(kAvLogRepeatTracked); i++) {
      REQUIRE(av_log_should_print(state, "msg " + std::to_string(i), i, kMinute, &suppressed));
    }
    REQUIRE(state.recent.size() == kAvLogRepeatTracked);
  }

  SECTION("the count resets once reported, so it is per interval not cumulative") {
    REQUIRE(av_log_should_print(state, "x", 0, kMinute, &suppressed));
    REQUIRE_FALSE(av_log_should_print(state, "x", 1, kMinute, &suppressed));
    REQUIRE(av_log_should_print(state, "x", kMinute, kMinute, &suppressed));
    REQUIRE(suppressed == 1);
    REQUIRE(av_log_should_print(state, "x", 2 * kMinute, kMinute, &suppressed));
    REQUIRE(suppressed == 0);
  }
}

TEST_CASE("xcoder_param_int", "[ffmpeg]") {
  // The shape the Options column actually holds.
  const std::string m28 = "out=hw:maxExtraHwFrameCnt=20:extendPoolSize=32";

  SECTION("reads a parameter from the middle and the end") {
    CHECK(xcoder_param_int(m28, "maxExtraHwFrameCnt") == 20);
    CHECK(xcoder_param_int(m28, "extendPoolSize") == 32);
  }

  SECTION("a key that is absent is not a zero") {
    // -1 rather than 0, because 0 is a meaningful value for extendPoolSize
    // and would read as "configured to nothing" rather than "not configured".
    CHECK(xcoder_param_int(m28, "missing") == -1);
    CHECK(xcoder_param_int("out=hw", "maxExtraHwFrameCnt") == -1);
    CHECK(xcoder_param_int("", "maxExtraHwFrameCnt") == -1);
  }

  SECTION("keeps the quotes the Options column stores") {
    CHECK(xcoder_param_int("'out=hw:extendPoolSize=32'", "extendPoolSize") == 32);
    CHECK(xcoder_param_int("\"out=hw:extendPoolSize=32\"", "extendPoolSize") == 32);
  }

  SECTION("a key that is a prefix of another does not match it") {
    // Searching for the name inside the string would return 32 for both.
    CHECK(xcoder_param_int("out=hw:extendPoolSizeExtra=32", "extendPoolSize") == -1);
    CHECK(xcoder_param_int("out=hw:xmaxExtraHwFrameCnt=9", "maxExtraHwFrameCnt") == -1);
  }

  SECTION("a value that is not a number is not half-read") {
    CHECK(xcoder_param_int("out=hw:extendPoolSize=abc", "extendPoolSize") == -1);
    CHECK(xcoder_param_int("out=hw:extendPoolSize=32x", "extendPoolSize") == -1);
    CHECK(xcoder_param_int("out=hw:extendPoolSize=", "extendPoolSize") == -1);
    CHECK(xcoder_param_int("out=hw:extendPoolSize=-4", "extendPoolSize") == -1);
  }

  SECTION("a flag with no value is skipped rather than confusing the scan") {
    CHECK(xcoder_param_int("out=hw:someflag:extendPoolSize=8", "extendPoolSize") == 8);
  }

  SECTION("zero is a value, not an absence") {
    CHECK(xcoder_param_int("out=hw:extendPoolSize=0", "extendPoolSize") == 0);
  }
}

TEST_CASE("decode_rate_worth_reporting", "[ffmpeg]") {
  const double budget = 66900;   // 66.9ms, a 14.95fps capture

  SECTION("a hair over budget is not worth saying") {
    // 67.2ms against 67.0ms, three parts in a thousand. The average is an EMA
    // and the budget comes from a smoothed capture rate; they do not agree to
    // that precision. 29% of a day's warnings on one monitor were under a
    // tenth over, and their decoder queue averaged 3.7 frames.
    CHECK_FALSE(decode_rate_worth_reporting(67200, 67000));
    CHECK_FALSE(decode_rate_worth_reporting(budget * 1.02, budget));
    CHECK_FALSE(decode_rate_worth_reporting(budget * 1.09, budget));
  }

  SECTION("a tenth over is where the queue starts to build") {
    CHECK(decode_rate_worth_reporting(budget * 1.11, budget));
    CHECK(decode_rate_worth_reporting(budget * 1.5, budget));
    // 96.3ms against 66.5ms, the case that really was starving the decoder.
    CHECK(decode_rate_worth_reporting(96300, 66500));
  }

  SECTION("under budget is never worth saying") {
    CHECK_FALSE(decode_rate_worth_reporting(budget * 0.5, budget));
    CHECK_FALSE(decode_rate_worth_reporting(budget, budget));
  }

  SECTION("no capture rate means no budget to compare against") {
    // get_capture_fps() returns 0 before the rate is known, and dividing by it
    // would make the budget infinite or zero rather than absent.
    CHECK_FALSE(decode_rate_worth_reporting(100000, 0));
    CHECK_FALSE(decode_rate_worth_reporting(100000, -1));
  }
}

TEST_CASE("analysis_should_pace at a few frames of slack", "[ffmpeg]") {
  // The threshold is now taken from the frame rate rather than a flat two
  // seconds, because pacing sleeps to the capture rate and so preserves
  // whatever lag it already has. At 15fps four frames is about 267ms.
  const int64_t stale = (1000000 / 15) * 4;
  const int burst = 8;

  SECTION("a lag inside the slack still paces") {
    CHECK(analysis_should_pace(0, burst, 100000, stale, false));
  }

  SECTION("a lag past the slack catches up instead of holding it") {
    // This is the case that used to pace: a third of a second behind is well
    // inside two seconds, so analysis slept and stayed a third of a second
    // behind indefinitely.
    CHECK_FALSE(analysis_should_pace(0, burst, 330000, stale, false));
  }

  SECTION("a second behind is nowhere near acceptable now") {
    CHECK_FALSE(analysis_should_pace(0, burst, 1000000, stale, false));
    CHECK_FALSE(analysis_should_pace(0, burst, 2000000, stale, false));
  }

  SECTION("hysteresis still applies, at half the new threshold") {
    // Catching up continues until well inside the slack, so a lag sitting on
    // the line does not flip every frame.
    CHECK_FALSE(analysis_should_pace(0, burst, stale * 3 / 4, stale, true));
    CHECK(analysis_should_pace(0, burst, stale / 4, stale, true));
  }

  SECTION("frames queued still burst regardless of age") {
    CHECK_FALSE(analysis_should_pace(burst + 1, burst, 0, stale, false));
  }
}

TEST_CASE("analysis_should_pace", "[ffmpeg]") {
  const int burst = 7;                       // image_buffer_count/4 on a 30-slot ring
  const int64_t stale = 2 * 1000 * 1000;     // 2s, as Monitor::Analyse uses
  const bool paced = false, catching = true;

  SECTION("keeping up: pace, so the analyser does not lap the streamers") {
    CHECK(analysis_should_pace(0, burst, 0, stale, paced));
    CHECK(analysis_should_pace(3, burst, 100000, stale, paced));
    CHECK(analysis_should_pace(burst, burst, 0, stale, paced));
  }

  SECTION("frames behind the decoder: burst") {
    CHECK_FALSE(analysis_should_pace(burst + 1, burst, 0, stale, paced));
    CHECK_FALSE(analysis_should_pace(100, burst, 0, stale, paced));
  }

  SECTION("seconds behind real time: burst, even with the decoder alongside") {
    // The case the frame counts cannot see. When the decoder is itself late
    // both counters advance together and decoder_lag stays small, so pacing
    // held the lag open until the queue overflowed.
    CHECK_FALSE(analysis_should_pace(0, burst, 2'100'000, stale, paced));
    CHECK_FALSE(analysis_should_pace(2, burst, 5'000'000, stale, paced));
  }

  SECTION("entering takes the full threshold") {
    CHECK(analysis_should_pace(0, burst, 1'900'000, stale, paced));
    CHECK(analysis_should_pace(0, burst, stale, stale, paced));
  }

  SECTION("leaving takes half, so a lag on the line does not flip every frame") {
    // m4 settled between 2.00 and 2.10s. With one threshold it crossed 2.4
    // times a second; with two it keeps catching up until it is under 1s.
    CHECK_FALSE(analysis_should_pace(0, burst, 1'900'000, stale, catching));
    CHECK_FALSE(analysis_should_pace(0, burst, 1'100'000, stale, catching));
    CHECK(analysis_should_pace(0, burst, 900'000, stale, catching));
  }

  SECTION("no staleness threshold leaves the frame-count behaviour alone") {
    CHECK(analysis_should_pace(0, burst, 10'000'000, 0, paced));
    CHECK(analysis_should_pace(0, burst, 10'000'000, 0, catching));
    CHECK_FALSE(analysis_should_pace(burst + 1, burst, 10'000'000, 0, paced));
  }
}

TEST_CASE("hw_jpeg_encoder_name", "[ffmpeg]") {
  SECTION("devices whose jpeg encoder is worth using") {
    REQUIRE(std::string(hw_jpeg_encoder_name(AV_HWDEVICE_TYPE_VAAPI)) == "mjpeg_vaapi");
    REQUIRE(std::string(hw_jpeg_encoder_name(AV_HWDEVICE_TYPE_QSV)) == "mjpeg_qsv");
  }
  SECTION("no device at all") {
    REQUIRE(hw_jpeg_encoder_name(AV_HWDEVICE_TYPE_NONE) == nullptr);
  }
#ifdef HAVE_QUADRA
  // The enum value only exists in NetInt's ffmpeg, so this can only be asserted
  // where that is what we built against.
  SECTION("NetInt Quadra is excluded deliberately") {
    // jpeg_ni_quadra_enc exists, but it costs the card far more than the jpegs
    // are worth. Returning it here would quietly opt every Quadra monitor in.
    REQUIRE(hw_jpeg_encoder_name(AV_HWDEVICE_TYPE_NI_QUADRA) == nullptr);
  }
#endif
  SECTION("a device with no jpeg encoder falls back to software") {
    REQUIRE(hw_jpeg_encoder_name(AV_HWDEVICE_TYPE_CUDA) == nullptr);
    REQUIRE(hw_jpeg_encoder_name(AV_HWDEVICE_TYPE_VDPAU) == nullptr);
  }
}
