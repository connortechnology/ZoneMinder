// Harness: drive our real RtpPacketizer with AV1 TUs over UDP loopback, exactly
// like the live AV1_ZoneMinderFifoSource, so we can validate the wire path with
// ffprobe/ffmpeg as the receiver — no zm_rtsp_server, no sudo.
//
// Toggle the keyframe flag with ZM_KEY=1/0 to demonstrate that the AV1 RTP N bit
// (set by ffmpeg's muxer only when is_keyframe) is what makes the depacketizer
// lock on. With ZM_KEY=0 it reproduces "before keyframe, dropping" forever.
//
// Build:
//   g++ -std=c++17 tests/av1_rtp_sender.cpp src/zm_rtp_packetizer.cpp -Isrc \
//       -o /tmp/av1send $(pkg-config --cflags --libs libavformat libavcodec libavutil)
// Run (writes /tmp/zmsend.sdp, loops forever):
//   ZM_KEY=1 /tmp/av1send /tmp/zmav1b.mp4 127.0.0.1 5020 /tmp/zmsend.sdp

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}
#include "zm_rtp_packetizer.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

enum { AV1_OBU_SEQUENCE_HEADER = 1, AV1_OBU_TEMPORAL_DELIMITER = 2 };

static size_t parseLEB128(const uint8_t *d, size_t max, uint32_t &v) {
  v = 0; size_t n = 0;
  for (size_t i = 0; i < max && i < 8; i++) { uint8_t b = d[i]; v |= ((uint32_t)(b & 0x7F)) << (7 * i); n++; if (!(b & 0x80)) break; }
  return n;
}
static size_t parseOBUHeader(const uint8_t *d, size_t size, uint8_t &type, bool &hs) {
  if (size < 1) return 0; uint8_t h = d[0]; if (h & 0x80) return 0;
  type = (h >> 3) & 0x0F; bool ext = (h >> 2) & 0x01; hs = (h >> 1) & 0x01;
  if (ext) return size < 2 ? 0 : 2; return 1;
}
static std::vector<uint8_t> seqHeaderFromAv1C(const uint8_t *ed, int size) {
  if (size <= 4 || (ed[0] & 0x80) == 0) return {};
  return std::vector<uint8_t>(ed + 4, ed + size);
}
// true if a SEQUENCE_HEADER OBU appears in this temporal unit (=> keyframe)
static bool tuHasSeqHeader(const uint8_t *p, size_t rem) {
  int guard = 0;
  while (rem > 0 && guard++ < 32) {
    uint8_t t; bool hs; size_t h = parseOBUHeader(p, rem, t, hs);
    if (!h) return false;
    if (t == AV1_OBU_SEQUENCE_HEADER) return true;
    if (!hs) return false;
    uint32_t v; size_t l = parseLEB128(p + h, rem - h, v); size_t obu = h + l + v;
    if (obu == 0 || obu > rem) return false;
    p += obu; rem -= obu;
  }
  return false;
}
static std::vector<uint8_t> combine(const std::vector<uint8_t> &seq, const uint8_t *frame, size_t frameSize) {
  uint8_t t = 0; bool hs = false; size_t h = parseOBUHeader(frame, frameSize, t, hs); size_t td = 0;
  if (h > 0 && t == AV1_OBU_TEMPORAL_DELIMITER) { if (hs) { uint32_t v; size_t l = parseLEB128(frame + h, frameSize - h, v); td = h + l + v; } else td = h; if (td > frameSize) td = 0; }
  std::vector<uint8_t> out;
  if (td > 0) out.insert(out.end(), frame, frame + td);
  out.insert(out.end(), seq.begin(), seq.end());
  out.insert(out.end(), frame + td, frame + frameSize);
  return out;
}

int main(int argc, char **argv) {
  if (argc < 5) { fprintf(stderr, "usage: %s file.mp4 host port sdpfile\n", argv[0]); return 2; }
  const char *file = argv[1], *host = argv[2]; int port = atoi(argv[3]); const char *sdpfile = argv[4];
  bool setKey = getenv("ZM_KEY") ? atoi(getenv("ZM_KEY")) : 1;
  av_log_set_level(AV_LOG_ERROR);

  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons(port); inet_pton(AF_INET, host, &dst.sin_addr);

  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, file, nullptr, nullptr) < 0) { fprintf(stderr, "open fail\n"); return 1; }
  avformat_find_stream_info(fmt, nullptr);
  int vs = -1; for (unsigned i = 0; i < fmt->nb_streams; i++) if (fmt->streams[i]->codecpar->codec_id == AV_CODEC_ID_AV1) { vs = i; break; }
  if (vs < 0) { fprintf(stderr, "no av1\n"); return 1; }
  AVCodecParameters *cp = fmt->streams[vs]->codecpar;
  std::vector<uint8_t> seq = seqHeaderFromAv1C(cp->extradata, cp->extradata_size);

  AVCodecParameters *pcp = avcodec_parameters_alloc();
  pcp->codec_type = AVMEDIA_TYPE_VIDEO; pcp->codec_id = AV_CODEC_ID_AV1;
  pcp->width = cp->width; pcp->height = cp->height; pcp->format = AV_PIX_FMT_YUV420P;

  RtpPacketizer pk;
  bool ok = pk.open(pcp, AVRational{1, 1000000}, 96, 1400, [&](const uint8_t *rtp, size_t size) {
    sendto(sock, rtp, size, 0, (sockaddr *)&dst, sizeof dst);
  });
  avcodec_parameters_free(&pcp);
  if (!ok) { fprintf(stderr, "packetizer open fail\n"); return 1; }
  FILE *sf = fopen(sdpfile, "w"); if (sf) { fputs(pk.sdp().c_str(), sf); fclose(sf); }
  fprintf(stderr, "ZM_KEY=%d, sdp written to %s, looping...\n", setKey, sdpfile);

  int64_t pts = 0; bool prepended = false;
  for (;;) {
    AVPacket *p = av_packet_alloc();
    while (av_read_frame(fmt, p) == 0) {
      if (p->stream_index == vs) {
        bool key = p->flags & AV_PKT_FLAG_KEY;
        std::vector<uint8_t> tu;
        if (key && !prepended) { tu = combine(seq, p->data, p->size); prepended = true; }
        else tu.assign(p->data, p->data + p->size);

        AVPacket *out = av_packet_alloc();
        out->data = tu.data(); out->size = (int)tu.size(); out->pts = out->dts = pts;
        if (setKey && tuHasSeqHeader(tu.data(), tu.size())) out->flags |= AV_PKT_FLAG_KEY;
        pk.packetize(out);
        out->data = nullptr; out->size = 0; av_packet_free(&out);
        pts += 100000;  // 10 fps in microseconds
        timespec ts{0, 100 * 1000 * 1000}; nanosleep(&ts, nullptr);
      }
      av_packet_unref(p);
    }
    av_packet_free(&p);
    av_seek_frame(fmt, vs, 0, AVSEEK_FLAG_BACKWARD);
  }
  return 0;
}
