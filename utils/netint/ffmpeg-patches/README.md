# NetInt FFmpeg patches

Patches we carry against NETINT's FFmpeg fork until they are merged
upstream. Both are in `vf_drawtext_ni.c` and both are needed for the
Quadra AI labelling path (`src/zm_netint_yolo.cpp` with
`SOFTWARE_DRAWBOX` at 0) to work at usable speed.

Target: Quadra V5.8.0, libxcoder `5806tCr2`, FFmpeg 7.x.
Applied with `patch -p1` from the FFmpeg source root, in `series` order.
They compile with no warnings beyond the three that tree already emits.

## 0001 — build the overlay frame pool in the in-place path

`init_hwframe_uploader()` builds the scaler session's output frame pool
only under `if (s->use_watermark)`, but `filter_frame()` takes an output
frame from that session on both paths. Without the pool,
`ni_scaler_session_read_hwdesc()` gets frame index 0 and spins up to
1000 times, issuing a device query on each pass. The crop session does
get a pool, sized 1, which allows one frame in flight and stalls the
same way.

Measured on monitor 4 (3840x2160 h264, one or two labels a frame),
before the patch:

    drawtext  76 calls  mean 2691.77ms  max 17469.20ms  20 blocked >250ms
    drawbox   90 calls  mean 5.96ms     max 10ms         0 blocked

Same graph, same frames. 26% of drawtext calls stalled between 6.2s and
17.5s, in two clusters: one exhausted retry loop, or two. `drawbox`
builds its pool unconditionally and never stalled.

After, on the same monitor and workload:

    drawtext  99 calls  mean 6.85ms     max 15.86ms      0 blocked >250ms
    drawbox   99 calls  mean 5.12ms

Mean 2691.77ms to 6.85ms, worst case 17469.20ms to 15.86ms, no stalls.
drawtext now costs about what drawbox costs. This is why we are not
upstreaming a software character blitter: the filter was never slow at
drawing, it was waiting for a frame.

## 0002 — mark the per-frame options as runtime parameters

Every option in `ni_drawtext_options[]` carries plain `FLAGS`. Upstream
`vf_drawtext.c` defines `TFLAGS` with `AV_OPT_FLAG_RUNTIME_PARAM` and
tags the per-frame set with it. Since FFmpeg 7.0 `av_opt_set()` and
`process_command()` refuse an option without that flag on an initialised
object, so the filter's own `reinit` command fails with EINVAL and the
text set at graph construction is the only text it will ever draw —
silently, with no error at the draw.

We work around this in `filter_worker::opt_set()`, which writes to
`filter_ctx->priv` where the check does not apply. That keeps us going
without the patch, but anything using `avfilter_graph_send_command()`
on this filter still needs it. `Quadra_Yolo::draw_text()` is the one
caller still on the command path.

## Dropping these

Once NETINT ships a release containing both, delete this directory and
the `series` file. Check `vf_drawtext_ni.c` for an unconditional
`ff_ni_build_frame_pool(&s->api_ctx, ...)` and for a `TFLAGS` define.
