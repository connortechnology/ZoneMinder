#include "zm_analysis_thread.h"

#include <algorithm>

#include "zm_monitor.h"
#include "zm_signal.h"
#include "zm_time.h"

AnalysisThread::AnalysisThread(Monitor *monitor) :
  monitor_(monitor), terminate_(false) {
  thread_ = std::thread(&AnalysisThread::Run, this);
  set_cpu_affinity(thread_);
}

AnalysisThread::~AnalysisThread() {
  Stop();
  if (thread_.joinable()) thread_.join();
}

void AnalysisThread::Start() {
  Stop();  // Signal any running thread to terminate first
  if (thread_.joinable()) thread_.join();
  terminate_ = false;
  Debug(3, "Starting analysis thread");
  thread_ = std::thread(&AnalysisThread::Run, this);
  set_cpu_affinity(thread_);
}

void AnalysisThread::Stop() {
  terminate_ = true;
}
void AnalysisThread::Join() {
  if (thread_.joinable()) thread_.join();
}

// Frames of slack before analysis stops pacing and catches up. Enough to ride
// out ordinary jitter, far short of the two seconds that used to be allowed to
// become permanent.
static constexpr int kAnalysisSlackFrames = 4;

void AnalysisThread::Run() {
  SystemTimePoint last_analysis_time = std::chrono::system_clock::now();

  // Analyse_Quadra is 23% of the frame interval and the reference blend 4%,
  // yet the analysis thread sits one to two seconds behind capture and only
  // holds there by skipping a quarter of its inferences. Either the rest of
  // Analyse() accounts for the difference, or the thread is idle and the lag
  // is where it reads from rather than how fast it reads. Time the whole call
  // and count the times there was nothing to do, which tells those apart.
  uint64_t analyse_us = 0, analyse_max_us = 0, analysed = 0, idle = 0;
  // Work plus wait came to 56% of the thread while the lag persisted, so a
  // third of each second is going somewhere neither timer sees. Measure the
  // gap between one Analyse() returning and the next starting: a tight loop
  // leaves nothing here, and anything large means the thread is held up
  // outside the call, which no amount of making Analyse() cheaper would fix.
  uint64_t between_us = 0, between_max_us = 0;
  // The lag itself, which is the thing being managed. Reading it from the AI
  // catch-up counters only works when there are detections to run, and a
  // quiet scene reports nothing either way -- twice now that has looked like
  // an improvement when it was only an empty field of view.
  uint64_t lag_us_total = 0, lag_max_us = 0, lag_samples = 0;
  SystemTimePoint last_analyse_end{};
  SystemTimePoint loop_reported = std::chrono::system_clock::now();

  while (!(terminate_ or zm_terminate)) {
    // Some periodic updates are required for variable capturing framerate
    SystemTimePoint analyse_start = std::chrono::system_clock::now();
    if (last_analyse_end.time_since_epoch().count()) {
      const uint64_t gap = std::chrono::duration_cast<Microseconds>(
          analyse_start - last_analyse_end).count();
      between_us += gap;
      if (gap > between_max_us) between_max_us = gap;
    }
    int ret = monitor_->Analyse();
    {
      const SystemTimePoint now = std::chrono::system_clock::now();
      last_analyse_end = now;
      const uint64_t us = std::chrono::duration_cast<Microseconds>(now - analyse_start).count();
      if (ret < 0) {
        idle++;
      } else {
        analyse_us += us;
        if (us > analyse_max_us) analyse_max_us = us;
        analysed++;
      }
      {
        const int64_t lag = monitor_->AnalysisLagUs();
        if (lag > 0) {
          lag_us_total += lag;
          if (static_cast<uint64_t>(lag) > lag_max_us) lag_max_us = lag;
          lag_samples++;
        }
      }
      if (FPSeconds(now - loop_reported).count() >= 60.0) {
        const double secs = FPSeconds(now - loop_reported).count();
        // Split the wait out of the total. Waiting is the decoder not having
        // finished; the remainder is what analysis actually costs.
        const uint64_t wait_us = monitor_->TakeAnalyseWaitUs();
        const uint64_t wait_max_us = monitor_->TakeAnalyseWaitMaxUs();
        const uint64_t work_us = analyse_us > wait_us ? analyse_us - wait_us : 0;
        Info("Analyse loop: %ju frames in %.0fs (%.1f/s), mean %.1fms of which "
             "%.1fms waiting for a decoded packet and %.1fms analysing; max "
             "%.1fms, longest wait %.1fms; work is %.0f%% of the thread, wait "
             "%.0f%%, and %.0f%% is between calls (mean %.1fms, max %.1fms); "
             "%ju passes had nothing to do; analysis lag mean %.0fms max %.0fms",
             static_cast<uintmax_t>(analysed), secs,
             analysed / secs,
             analysed ? analyse_us / 1000.0 / analysed : 0.0,
             analysed ? wait_us / 1000.0 / analysed : 0.0,
             analysed ? work_us / 1000.0 / analysed : 0.0,
             analyse_max_us / 1000.0,
             wait_max_us / 1000.0,
             work_us / 10000.0 / secs,
             wait_us / 10000.0 / secs,
             between_us / 10000.0 / secs,
             analysed ? between_us / 1000.0 / analysed : 0.0,
             between_max_us / 1000.0,
             static_cast<uintmax_t>(idle),
             lag_samples ? lag_us_total / 1000.0 / lag_samples : 0.0,
             lag_max_us / 1000.0);
        analyse_us = analyse_max_us = analysed = idle = 0;
        between_us = between_max_us = 0;
        lag_us_total = lag_max_us = lag_samples = 0;
        loop_reported = now;
      }
    }
    if (ret < 0) {
      if (!(terminate_ or zm_terminate)) {
        // We wait on the packetqueue condition variable instead of sleeping.
        // This allows us to wake up immediately when decoding completes.
        Microseconds wait_for = monitor_->Active() ? Microseconds(ZM_SAMPLE_RATE) : Microseconds(ZM_SUSPENDED_RATE);
        Debug(5, "Waiting for %" PRId64 "us", int64(wait_for.count()));
        monitor_->GetPacketQueue()->wait_for(wait_for);
      }
      last_analysis_time = std::chrono::system_clock::now();
      continue;
    }

    // Backpressure: pace analysis to the camera's capture rate when we're
    // caught up with the decoder. Without this, after a stall drains the
    // packet queue the analyser writes analysis_image_buffer at burst rate
    // and laps streamers reading from it — they fall outside the ring-buffer
    // window and lose their slot ("Fell behind, maybe increase Image Buffers"
    // in zm_monitorstream.cpp). Allow a buffer-proportional burst (no sleep)
    // while the decoder is more than burst_lag frames ahead so we can still
    // catch up from a real backlog. capture_fps is EMA-smoothed in
    // Monitor::UpdateFPS so the rate we throttle to reflects the steady
    // state, not a transient drain spike.
    const int burst_lag = std::max(2, monitor_->GetImageBufferCount() / 4);
    const int decoder_lag =
        monitor_->shared_data->decoder_image_count -
        monitor_->shared_data->analysis_image_count;
    // Stale by more than this and we stop pacing and work through it. Matches
    // the threshold Monitor::Analyse uses to decide the AI has fallen behind,
    // so the two agree about what "behind" means.
    // How far behind real time analysis may sit before it stops pacing and
    // catches up. Pacing sleeps until a frame interval has passed, so it holds
    // whatever lag it already has: a hiccup that puts analysis a second behind
    // keeps it a second behind for as long as it paces. A flat two seconds
    // meant m4 sat between 1.0 and 2.1s indefinitely, skipping a quarter of
    // its inferences to stay there, on a thread that was sleeping 38% of the
    // time.
    //
    // Take it from the frame rate instead. A few frames of slack absorbs
    // jitter without pacing a standing lag into place, and detection then runs
    // as close to real time as the pipeline allows, which is the point of
    // doing it at all.
    const double pace_fps = monitor_->get_capture_fps();
    const int64_t frame_interval_us =
        (pace_fps > 0) ? static_cast<int64_t>(1e6 / pace_fps) : 66'666;
    const int64_t kStaleAfterUs = frame_interval_us * kAnalysisSlackFrames;
    const bool pace = analysis_should_pace(decoder_lag, burst_lag,
                                          monitor_->AnalysisLagUs(),
                                          kStaleAfterUs, catching_up_);
    catching_up_ = !pace;
    if (pace) {
      const double fps = monitor_->get_capture_fps();
      if (fps > 0) {
        const Microseconds target_interval(static_cast<int64_t>(1e6 / fps));
        const SystemTimePoint now = std::chrono::system_clock::now();
        const Microseconds elapsed =
            std::chrono::duration_cast<Microseconds>(now - last_analysis_time);
        if (elapsed < target_interval) {
          Microseconds sleep_for = target_interval - elapsed;
          // Cap the pacing sleep so an anomalously low capture_fps reading
          // can't paralyze us. Monitor::connect() zeroes capture_image_count
          // on every reconnect, and UpdateFPS then computes a sample over a
          // window that includes the dead air before the first new frame
          // (observed: 1 frame / 2.5s -> 0.4fps). Combined with alpha=0.05
          // EMA recovery, get_capture_fps() can sit below 5fps for 20+s
          // after a reconnect. target_interval = 1e6/0.4 = 2.5s would let
          // the packet queue overflow long before we wake.
          //
          // 30ms is a frame interval at ~33fps — under that rate the cap
          // shortens the pace cycle (harmless, just more loop iterations
          // because Analyse will return EAGAIN); above it the cap rarely
          // fires under sane EMA. Worst case at 100fps the cap lets the
          // queue grow by 3 frames per cycle before burst_lag triggers
          // catch-up, which fits comfortably in the default queue size.
          constexpr Microseconds kMaxPaceSleep(30'000);
          if (sleep_for > kMaxPaceSleep) sleep_for = kMaxPaceSleep;
          Debug(4,
                "Pacing analysis: sleeping %" PRId64
                "us (decoder_lag=%d, fps=%.1f)",
                int64(sleep_for.count()), decoder_lag, fps);
          std::this_thread::sleep_for(sleep_for);
        }
      }
    } else {
      Debug(4, "Bursting analysis: decoder_lag=%d > burst_lag=%d",
            decoder_lag, burst_lag);
    }
    last_analysis_time = std::chrono::system_clock::now();
  }
}
