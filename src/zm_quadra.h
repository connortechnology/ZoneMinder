#ifndef ZM_QUADRA_H
#define ZM_QUADRA_H

#if HAVE_QUADRA

#include "zm_ffmpeg.h"

#include "nierrno.h"
#include "ni_device_api.h"
#include "ni_rsrc_api.h"
#include "ni_util.h"

#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

/* Card-wide occupancy, as libxcoder's own shared resource pool reports it.
 *
 * This is the card's accounting, not ours. Every process using the card
 * publishes into the same pool under /dev/shm, so one zmc can see what all of
 * them add up to -- which the device frame gauge in zm_ffmpeg cannot, since it
 * only ever counts this process, and zmc is one process per monitor.
 *
 * Reading it is what we have instead of a guess. Both the upload pool size and
 * the device frame budget are constants picked by watching a single monitor,
 * and the card runs out of memory well before it runs out of compute, so the
 * first thing worth knowing is how far off those constants are.
 */
namespace zm_quadra {

// One hardware block on one card. A card carries a decoder, an encoder, a
// scaler and an AI block, each with its own instances and load.
struct BlockUsage {
  ni_device_type_t type = NI_DEVICE_TYPE_DECODER;
  int card_idx = -1;
  int load = -1;         // percent, as the firmware reports it
  int model_load = -1;   // percent, as libxcoder models it
  unsigned int active_instances = 0;
  int max_instances = -1;
  // Pixels summed over the active instances. Card memory goes on frames, and a
  // frame costs width*height*1.5 bytes per pool slot, so this -- not a count of
  // instances -- is the quantity a memory budget has to be written against.
  uint64_t active_pixels = 0;
};

// Reads the resource pool for one block type, one entry per card. Empty when
// the pool cannot be read, which includes the caller lacking permission on it;
// that is logged once and then left alone.
std::vector<BlockUsage> block_usage(ni_device_type_t type);

// A one-line summary for logging. Values the card did not report print as "?"
// rather than as a zero that would read as "idle".
std::string describe(const BlockUsage &usage);

}  // namespace zm_quadra

class Quadra {
  public:
    class filter_worker {
      public:
        AVFilterContext *buffersink_ctx;
        AVFilterContext *buffersrc_ctx;
        AVFilterGraph *filter_graph;
        filter_worker()  :
          buffersink_ctx(nullptr),
          buffersrc_ctx(nullptr),
          filter_graph(nullptr)
      {};
        ~filter_worker() {
          if (filter_graph) {
            avfilter_graph_free(&filter_graph);
            filter_graph = nullptr;
          }
        };
        AVFilterContext * find_filter_ctx(const char *name) {
          for (unsigned int i = 0; i < filter_graph->nb_filters; i++) {
            if (strstr(filter_graph->filters[i]->name, name) != nullptr) {
              return filter_graph->filters[i];
            }
          }
          return nullptr;
      };
    };

  private:
    av_frame_ptr scaled_frame;
    //SWScale swscale;
    SwsContext *sw_scale_ctx;

    filter_worker *drawbox_filter;
    filter_worker *hwdl_filter;
    AVFilterContext *drawbox_filter_ctx;

    bool use_hwframe;

  public:
    Quadra();
    ~Quadra();
    bool  setup(int deviceid=-1);
    bool  setup_drawbox(AVPixelFormat pixfmt, int width, int height);
    int   init_filter(const char *filters_desc, filter_worker *f, bool hwmode, int, int, AVPixelFormat in_ipxfmt);
    int draw_box(AVFrame *inframe, AVFrame **outframe, int x, int y, int w, int h, const std::string &colour);
  private:
    int dlhw_frame(AVFrame *hwframe, AVFrame **filt_frame);
};

#endif
#endif
