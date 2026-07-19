// Standalone harness to iterate on AV1 OBU framing for the RTSP restream WITHOUT
// the live zm_rtsp_server (no sudo install / restart loop).
//
// It mimics the live path in AV1_ZoneMinderFifoSource:
//   - the encoder uses global-header, so the sequence header arrives separately
//     (here: the mp4 av1C extradata -> raw seq-header OBU)
//   - each temporal unit (here: each demuxed AVPacket) is fed to RTP/decoder
//     with the seq header prepended (after the leading temporal delimiter) on
//     the first keyframe.
//
// It then feeds the combined temporal unit to libavcodec's AV1 decoder and
// reports whether it decodes and what resolution it reports — exactly the thing
// that fails over the wire today.
//
// Build:
//   g++ -std=c++17 tests/av1_framing_harness.cpp -o /tmp/av1harness \
//       $(pkg-config --cflags --libs libavformat libavcodec libavutil)
// Run:
//   /tmp/av1harness /tmp/zmav1.mp4

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

enum {
  AV1_OBU_SEQUENCE_HEADER = 1,
  AV1_OBU_TEMPORAL_DELIMITER = 2,
  AV1_OBU_FRAME = 6,
};

// --- mirror of the helpers in zm_rtsp_server_fifo_av1_source.cpp -------------
static size_t parseLEB128(const uint8_t *data, size_t max, uint32_t &value) {
  value = 0;
  size_t n = 0;
  for (size_t i = 0; i < max && i < 8; i++) {
    uint8_t b = data[i];
    value |= ((uint32_t)(b & 0x7F)) << (7 * i);
    n++;
    if (!(b & 0x80)) break;
  }
  return n;
}
static size_t parseOBUHeader(const uint8_t *d, size_t size, uint8_t &type, bool &has_size) {
  if (size < 1) return 0;
  uint8_t h = d[0];
  if (h & 0x80) return 0;
  type = (h >> 3) & 0x0F;
  bool ext = (h >> 2) & 0x01;
  has_size = (h >> 1) & 0x01;
  if (ext) { if (size < 2) return 0; return 2; }
  return 1;
}

// Walk and print the OBU layout of a buffer.
static void dumpOBUs(const char *label, const uint8_t *p, size_t rem) {
  printf("  %s (%zu bytes):", label, rem);
  int guard = 0;
  while (rem > 0 && guard++ < 32) {
    uint8_t t; bool hs;
    size_t h = parseOBUHeader(p, rem, t, hs);
    if (!h) { printf(" <hdr-parse-fail>"); break; }
    size_t obu = rem;
    if (hs) { uint32_t v; size_t l = parseLEB128(p + h, rem - h, v); obu = h + l + v; }
    printf(" [type=%u hassize=%d len=%zu]", t, hs ? 1 : 0, obu);
    if (obu == 0 || obu > rem) { printf(" <bad-len>"); break; }
    p += obu; rem -= obu;
  }
  printf("\n");
}

// Replicate the live combine: insert seq header after a leading temporal delimiter.
static std::vector<uint8_t> combine(const std::vector<uint8_t> &seq,
                                    const uint8_t *frame, size_t frameSize) {
  uint8_t t = 0; bool hs = false;
  size_t h = parseOBUHeader(frame, frameSize, t, hs);
  size_t td_len = 0;
  if (h > 0 && t == AV1_OBU_TEMPORAL_DELIMITER) {
    if (hs) { uint32_t v; size_t l = parseLEB128(frame + h, frameSize - h, v); td_len = h + l + v; }
    else td_len = h;
    if (td_len > frameSize) td_len = 0;
  }
  std::vector<uint8_t> out;
  if (td_len > 0) out.insert(out.end(), frame, frame + td_len);
  out.insert(out.end(), seq.begin(), seq.end());
  out.insert(out.end(), frame + td_len, frame + frameSize);
  return out;
}

// Extract the raw configOBUs (the seq-header OBU) from an av1C extradata box.
static std::vector<uint8_t> seqHeaderFromAv1C(const uint8_t *ed, int size) {
  // av1C: 4-byte header, then configOBUs (raw low-overhead OBUs).
  if (size <= 4 || (ed[0] & 0x80) == 0) return {};  // marker bit must be set
  return std::vector<uint8_t>(ed + 4, ed + size);
}

static bool decodeBuffer(AVCodecContext *dec, const uint8_t *data, size_t size,
                         int &w, int &h) {
  AVPacket *pkt = av_packet_alloc();
  pkt->data = const_cast<uint8_t *>(data);
  pkt->size = (int)size;
  int ret = avcodec_send_packet(dec, pkt);
  pkt->data = nullptr; pkt->size = 0;
  av_packet_free(&pkt);
  if (ret < 0) { char e[128]; av_strerror(ret, e, sizeof e); printf("  send_packet: %s\n", e); return false; }
  AVFrame *fr = av_frame_alloc();
  bool got = false;
  while (avcodec_receive_frame(dec, fr) == 0) { w = fr->width; h = fr->height; got = true; }
  av_frame_free(&fr);
  return got;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s file.mp4\n", argv[0]); return 2; }
  av_log_set_level(AV_LOG_ERROR);

  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, argv[1], nullptr, nullptr) < 0) { fprintf(stderr, "open fail\n"); return 1; }
  avformat_find_stream_info(fmt, nullptr);
  int vs = -1;
  for (unsigned i = 0; i < fmt->nb_streams; i++)
    if (fmt->streams[i]->codecpar->codec_id == AV_CODEC_ID_AV1) { vs = (int)i; break; }
  if (vs < 0) { fprintf(stderr, "no av1 stream\n"); return 1; }

  AVCodecParameters *cp = fmt->streams[vs]->codecpar;
  printf("av1 stream %dx%d, extradata %d bytes\n", cp->width, cp->height, cp->extradata_size);
  std::vector<uint8_t> seq = seqHeaderFromAv1C(cp->extradata, cp->extradata_size);
  dumpOBUs("seq header OBU (from av1C)", seq.data(), seq.size());

  // Decoder fed ONLY in-band data (no extradata) — exactly like a wire client
  // that has never seen the av1C box.
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
  AVCodecContext *dec = avcodec_alloc_context3(codec);
  if (avcodec_open2(dec, codec, nullptr) < 0) { fprintf(stderr, "decoder open fail (%s)\n", codec ? codec->name : "?"); return 1; }
  printf("decoder: %s\n\n", codec->name);

  AVPacket *pkt = av_packet_alloc();
  int n = 0, ok = 0;
  bool prepended = false;
  while (av_read_frame(fmt, pkt) == 0 && n < 12) {
    if (pkt->stream_index == vs) {
      bool key = pkt->flags & AV_PKT_FLAG_KEY;
      printf("TU %d (key=%d, %d bytes):\n", n, key, pkt->size);
      dumpOBUs("raw TU", pkt->data, pkt->size);

      std::vector<uint8_t> tu;
      if (key && !prepended) {
        tu = combine(seq, pkt->data, pkt->size);
        prepended = true;
        dumpOBUs("combined (seq prepended)", tu.data(), tu.size());
      } else {
        tu.assign(pkt->data, pkt->data + pkt->size);
      }
      int w = 0, h = 0;
      bool got = decodeBuffer(dec, tu.data(), tu.size(), w, h);
      printf("  decode: %s  resolution=%dx%d\n", got ? "OK" : "no-frame", w, h);
      if (got && w > 0) ok++;
      n++;
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  avcodec_free_context(&dec);
  avformat_close_input(&fmt);
  printf("\n%d/%d TUs decoded with a resolution\n", ok, n);
  return ok > 0 ? 0 : 1;
}
