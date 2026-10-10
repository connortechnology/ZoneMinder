#ifndef ZM_VIDEOSTORE_H
#define ZM_VIDEOSTORE_H

#include "zm_config.h"
#include "zm_define.h"
#include "zm_ffmpeg.h"
#include "zm_swscale.h"

#include <list>
#include <memory>
#include <map>
#include <string>
#include <vector>

extern "C"  {
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
#if HAVE_LIBAVUTIL_HWCONTEXT_H
#include <libavutil/hwcontext.h>
#endif
#include "libavutil/buffer.h"
}

class Monitor;
class ZMPacket;
class PacketQueue;

class VideoStore {
 public:
  struct Fragment {
    int64_t offset;    // byte offset in file
    int64_t size;      // bytes (moof+mdat)
    double duration;   // seconds
  };

 private:

  const CodecData *chosen_codec_data;

  Monitor *monitor;
  AVOutputFormat *out_format;
  AVFormatContext *oc;
  AVStream *video_out_stream;
  AVStream *audio_out_stream;

  AVCodecContext *video_in_ctx;
  AVCodecContext *video_out_ctx;

  AVStream *video_in_stream;
  AVStream *audio_in_stream;

  const AVCodec *audio_in_codec;
  AVCodecContext *audio_in_ctx;
  // The following are used when encoding the audio stream to AAC
  const AVCodec *audio_out_codec;
  AVCodecContext *audio_out_ctx;
  // Move this into the object so that we aren't constantly allocating/deallocating it on the stack
  av_packet_ptr opkt;

  av_frame_ptr in_frame;
  av_frame_ptr out_frame;

  SWScale swscale;
  unsigned int packets_written;
  unsigned int frame_count;
  int64_t encode_total_us_;
  int encode_count_;
  bool video_encoded;  // true once at least one frame has been sent to the video encoder
  bool video_encoder_failed;  // true after a fatal encoder error; skip further sends
  // True when video_out_ctx draws frames from the decoder's own pool. Only then
  // may a decoded device frame be handed to the encoder: a VAAPI encoder accepts
  // a surface from a pool it does not own and encodes black without any error,
  // and because both are AV_PIX_FMT_VAAPI the upload path does not trigger
  // either, so nothing else catches it.
  bool shares_decoder_pool;
  // Set in open() when the monitor is configured to ENCODE but no encoder could
  // be opened; we then copy the input stream and write packets unchanged.
  bool video_passthrough_fallback;

  AVBufferRef *hw_device_ctx;
  // Where an upload goes while shares_decoder_pool is set: it must not allocate
  // out of the decoder's pool, since the surface it took would be one the
  // decoder is counting on. Created on the first upload.
  AVBufferRef *upload_frames_ctx;

  SwrContext *resample_ctx;
  AVAudioFifo *fifo;
  uint8_t *converted_in_samples;

  // filename is owned (std::string) so it stays valid for the lifetime of
  // VideoStore even if the caller later renames/reassigns the source path
  // it was constructed from. A bare const char* would dangle in that case.
  std::string filename;
  const char *format;

  // These are for in
  int64_t video_first_pts; /* starting pts of first in frame/packet */
  int64_t video_first_dts;
  int64_t audio_first_pts;
  int64_t audio_first_dts;
  int64_t video_last_pts;
  int64_t audio_last_pts;

  // These are for out, should start at zero.  We assume they do not wrap because we just aren't going to save files that big.
  int64_t *next_dts;
  std::map<int, int64_t> last_dts;
  std::map<int, int64_t> last_duration;
  int64_t audio_next_pts;

  int max_stream_index;

  size_t reorder_queue_size;
  std::map<int, std::list<std::shared_ptr<ZMPacket>>> reorder_queues;

  // HLS fragment tracking. With movflags=frag_keyframe, FFmpeg's mov muxer
  // doesn't write a fragment to disk until the *next* keyframe arrives (or
  // until av_write_trailer is called). So when keyframe N arrives, fragment
  // N-1 is what just got flushed. We snapshot avio_tell *after*
  // av_interleaved_write_frame() to capture the position past that flush, and
  // record fragment N-1 then.
  std::vector<Fragment> fragments_;
  int64_t last_fragment_offset_;    // byte offset where the current (in-progress) fragment starts
  int64_t last_fragment_start_dts_; // DTS of the keyframe that started the current fragment
  int64_t init_segment_end_;        // byte offset where init segment (ftyp+moov) ends
  // Where open() reserved room for a leading sidx, or -1 when it did not.
  // finalize() fills that region in; zm_mp4_sidx.h says why the index has to
  // sit there and be reserved before the first fragment is written.
  int64_t sidx_region_offset_;
  int64_t sidx_region_size_;        // how many bytes open() reserved there
  bool    finalized_;               // true once finalize() has run trailer + last-fragment recording

  // Sticky flag: once av_interleaved_write_frame fails, muxer state may be
  // inconsistent and a subsequent call can trigger an internal ffmpeg abort.
  bool write_packet_failed_;

  // Set while finalize() drains the reorder queues + writes the trailer. The
  // fragmented-mp4 muxer can abort() (not just error) here on inconsistent
  // timestamps (movenc get_cluster_duration av_assert0). When set, write_packet
  // logs each packet's timestamps just before the mux call so the last log line
  // before such an abort identifies the offending stream/packet.
  bool finalizing_ = false;

  // What of the manifest is already on disk, and what it was written with, so
  // writeM3U8 can add the new fragments to the end instead of rewriting it.
  // Zeroed state means "rewrite from scratch", which is also the self-healing
  // answer to anything unexpected.
  size_t      m3u8_fragments_written_;
  int         m3u8_target_duration_;
  int64_t     m3u8_init_segment_end_;
  int64_t     m3u8_bytes_written_;
  std::string m3u8_path_;
  std::string m3u8_video_url_;

  bool setup_resampler();
  int write_packet(AVPacket *pkt, AVStream *stream);
  // Pull one packet from the video encoder and route it through write_packet.
  // The ONLY correct way to drain encoder output — discarding packets here
  // (older code did) silently breaks recording on any encoder with non-zero
  // reorder latency, because every shown frame ends up coming out during the
  // next iteration's pre-send drain instead of the post-send receive loop.
  int receive_and_write_video_packet();

 public:
  VideoStore(
    const char *filename_in,
    const char *format_in,
    AVStream *video_in_stream,
    AVCodecContext  *video_in_ctx,
    AVStream *audio_in_stream,
    Monitor * p_monitor);
  ~VideoStore();
  bool open();
  // Builds upload_frames_ctx on first use; only called when the encoder is on
  // the decoder's pool. Returns 0 or an AVERROR.
  int alloc_upload_pool();

  void write_video_packet(AVPacket pkt);
  void write_audio_packet(AVPacket pkt);
  int writeVideoFramePacket(const std::shared_ptr<ZMPacket> pkt);
  int writeAudioFramePacket(const std::shared_ptr<ZMPacket> pkt);
  int writePacket(const std::shared_ptr<ZMPacket> pkt);
  int write_packets(PacketQueue &queue);
  void flush_codecs();
  const std::vector<Fragment> &fragments() const { return fragments_; }
  int64_t init_segment_end() const { return init_segment_end_; }
  void writeM3U8(const std::string &path, const std::string &video_url, bool is_complete);

  // --- Manifest text, split out so it is testable without a VideoStore ------
  //
  // An EVENT playlist is append only: the lines for a fragment never change
  // once written, and only the header depends on anything global. So the
  // header and a fragment's three lines are generated separately, and
  // writeM3U8 appends when nothing above the new fragments would differ.

  // Rounded up, and never below 1, as EXT-X-TARGETDURATION must be a positive
  // integer no smaller than any fragment.
  static int m3u8TargetDuration(const std::vector<Fragment> &frags);
  static std::string m3u8Header(int target_duration,
                                const std::string &video_url,
                                int64_t init_segment_end,
                                bool is_complete);
  static std::string m3u8Fragment(const Fragment &frag, const std::string &video_url);
  // Flush queues, write trailer, close output, and record the final fragment.
  // Call this before writeM3U8(true) so the manifest contains every fragment.
  // Safe to call once; subsequent calls are no-ops. The destructor will skip
  // the trailer write if finalize() has already run.
  void finalize();

  // True when we are actually re-encoding video. This is the monitor's ENCODE
  // setting unless every encoder failed to open, in which case open() falls
  // back to passthrough and this returns false. Defined in the .cpp because
  // Monitor is only forward-declared here.
  bool Encoding() const;

  const char *get_codec() {
    if (chosen_codec_data)
      return chosen_codec_data->codec_codec;
    if (video_out_stream)
      return avcodec_get_name(video_out_stream->codecpar->codec_id);
    return "";
  }
  const AVCodec * get_video_encoder() {
    return video_out_ctx ? video_out_ctx->codec : nullptr;
  }
  size_t get_reorder_queue_size() const { return reorder_queue_size; };

  // Keep our path in sync when the caller renames the on-disk file out from
  // under the open AVFormatContext. FFmpeg's faststart trailer pass re-opens
  // oc->url by name, and finalize() fopen()s filename to read the mfra box, so
  // both must track the new name or they fail with ENOENT.
  void set_filename(const std::string &new_filename);
};

#endif // ZM_VIDEOSTORE_H

