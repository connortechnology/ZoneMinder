# NetInt FFmpeg patches

Patches we carry against NETINT's FFmpeg fork until they are merged
upstream. All three are in `vf_drawtext_ni.c`, and the Quadra AI
labelling path (`src/zm_netint_yolo.cpp` with `SOFTWARE_DRAWBOX` at 0)
needs all three: without 0001 it stalls, without 0002 and 0003 it draws
the text it was built with and never changes it.

Target: Quadra V5.8.0, libxcoder `5806tCr2`, FFmpeg 8.1.
Applied with `patch -p1` from the FFmpeg source root, in `series` order.
They compile with no warnings beyond the three that tree already emits.

## 0001 — build the overlay frame pool in the in-place path

`init_hwframe_uploader()` builds the scaler session's output frame pool
only under `if (s->use_watermark)`, but `filter_frame()` takes an output
frame from that session on both paths. Without the pool,
`ni_scaler_session_read_hwdesc()` gets frame index 0 and spins up to
1000 times, issuing a device query on each pass.

The size is a `pool_size` option defaulting to
`DEFAULT_NI_FILTER_POOL_SIZE`, so nothing changes for an existing
caller. Four frames is not enough for a caller holding device frames of
its own, and `ctx->extra_hw_frames` is not the way to raise it: that
field is libavfilter's own accounting, fixed when the filter is
initialised, and a pool larger than it leaves the frames this filter
emits outside the hardware frames context the next filter was
configured for. `hwdownload` then refuses every one with EINVAL. We
tried that first and it broke the path completely.

Measured on monitor 4 (3840x2160 h264, one or two labels a frame),
before the patch:

    drawtext  76 calls  mean 2691.77ms  max 17469.20ms  20 blocked >250ms
    drawbox   90 calls  mean    5.96ms  max     10ms     0 blocked

Same graph, same frames. 26% of drawtext calls stalled between 6.2s and
17.5s, in two clusters: one exhausted retry loop, or two. `drawbox`
builds its pool unconditionally and never stalled.

After, with `pool_size` set from the monitor's frame budget, over a
longer run and with the labels actually being drawn:

    drawtext 2399 calls  mean 7.12ms  max 22.90ms  0 blocked >250ms
    drawbox  2399 calls  mean 5.06ms

Mean 2691.77ms to 7.12ms, worst case 17469.20ms to 22.90ms, no stalls.
drawtext now costs about what drawbox costs.

`ni_rsrc_mon` is what settled the diagnosis. Throughout the stalls the
card was idle:

    scaler   LOAD 0   MODEL_LOAD 0   INST 7   MEM 0   SHARE_MEM 47
    decoder  LOAD 11  MODEL_LOAD 16  INST 10  MEM 12  SHARE_MEM 47

0% scaler load and 88% of frame memory free, while the filter spun
unable to get a frame index. The card was never short of frames. One
session's pool was.

This is also why we are not upstreaming a software character blitter to
FFmpeg, which is where this work started: the filter was never slow at
drawing, it was waiting for a frame.

## 0002 — mark the per-frame options as runtime parameters

Every option in `ni_drawtext_options[]` carries plain `FLAGS`. Upstream
`vf_drawtext.c` defines `TFLAGS` with `AV_OPT_FLAG_RUNTIME_PARAM` and
tags the per-frame set with it. Since FFmpeg 7.0 `av_opt_set()` and
`process_command()` refuse an option without that flag on an initialised
object, so a command carrying them fails with EINVAL and the text set at
graph construction is the only text the filter will ever draw —
silently, with no error at the draw itself.

Writing to `filter_ctx->priv` directly, where the check does not apply,
is not a way round this. It reaches the option storage but not what
`init()` derives from it: `text_num` is counted once, and x and y are
evaluated from the parsed `x_pexpr`/`y_pexpr` rather than the strings.
We ran that way for a while and it drew nothing at all while reporting
success.

The box options (`box`, `bc0..bc31`, `bb0..bb31`) are deliberately not
retagged. They never change per frame, so we set them when the graph is
built, and `command()` copies the live options before applying its own
string, which carries them across.

## 0003 — an update command that does not close the scaler sessions

`reinit` is the only way to change the text, and it builds a fresh
context and `uninit()`s the old one, closing both scaler sessions and
rebuilding both frame pools. Measured at 45ms a call against 7ms for the
draw alone.

`update` applies the option string to the live context and re-derives
only what those options feed — `text_num`, the position expressions, and
the face and glyphs of a slot used for the first time — leaving the
sessions alone.

This is a cost fix, not a correctness one. The stalls in 0001 were the
undersized pool, not this churn: with the pool sized and `reinit` still
in use, 74 calls blocked against 12 reinits, so the blocking was
overwhelmingly on calls that sent no command. 0003 on its own will not
stop a filter starving.

## Dropping these

Once NETINT ships a release containing all three, delete this directory
and the `series` file, and drop `pool_size=` from the filter string in
`Quadra_Yolo::setup()`. Check `vf_drawtext_ni.c` for an unconditional
`ff_ni_build_frame_pool(&s->api_ctx, ...)`, for a `TFLAGS` define, and
for `update` in `command()`.
