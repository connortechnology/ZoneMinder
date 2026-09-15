#ifndef ZM_AVFILTER_WORKER_H
#define ZM_AVFILTER_WORKER_H

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include <libavutil/frame.h>
#include <libavutil/opt.h>

#include <libswscale/swscale.h>
}

class filter_worker {
  public:
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
    AVFilterContext *filter_ctx;
    AVCodecContext *dec_ctx;
    AVRational time_base;
    bool initialised;
    // The hardware frames context setup() was given, kept referenced so the
    // pointer stays valid to compare against.
    AVBufferRef *built_hw_frames_ctx;
    // How many times this filter has been rebuilt for a different context.
    // Rebuilding once as the graph settles is expected; rebuilding per frame
    // would mean two callers alternating and wanting a filter each.
    unsigned int rebuilds;

    filter_worker();
    ~filter_worker();
    bool setup(const std::string &filter_desc, const std::string &filter_of_interest, AVCodecContext *ctx, AVRational tbase, AVBufferRef *hw_frames_ctx, AVPixelFormat pix_fmt);
    // True when this filter was built against the same hardware frames
    // context the frame carries. A hwframe-aware filter only accepts frames
    // from the context it was configured with, and one filter shared between
    // callers whose frames come from different pools will reject whichever
    // did not get there first.
    bool built_for(const AVBufferRef *hw_frames_ctx) const;
    int execute(AVFrame *in_frame, AVFrame **out_frame);

    int opt_set(const std::string &opt, const std::string &value);
    int opt_set(const std::string &opt, int value);

    int send_command(const char *filter_name, const char *command, const char *option);
    int init_filter(const char *filters_desc, AVBufferRef * 	hw_frames_ctx, AVPixelFormat in_ipxfmt);
};

#endif
