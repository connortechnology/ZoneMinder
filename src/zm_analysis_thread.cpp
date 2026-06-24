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

void AnalysisThread::Run() {
  SystemTimePoint last_analysis_time = std::chrono::system_clock::now();

  while (!(terminate_ or zm_terminate)) {
    // Some periodic updates are required for variable capturing framerate
    int ret = monitor_->Analyse();
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
    if (decoder_lag <= burst_lag) {
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
