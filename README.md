# score-addon-lavfi

FFmpeg [libavfilter](https://ffmpeg.org/ffmpeg-filters.html) graphs as
[ossia score](https://ossia.io) processes.

One process, **FFmpeg filter**, whose program is a filtergraph string in the
same syntax as `ffmpeg -filter_complex`:

```
gblur=sigma=4,hue=h=90
[a][b]blend=all_mode=addition
showspectrum=s=1024x512:mode=combined
loudnorm=I=-16
testsrc=size=1280x720:rate=60
```

The process's ports are derived from the graph:

| port | from |
|---|---|
| audio / texture inlets | the graph's open input pads |
| audio / texture outlets | the graph's open output pads |
| control inlets `filter/option` | every option flagged runtime-changeable on any filter instance, forwarded with `avfilter_process_command` |
| `Metadata` value outlet | the `lavfi.*` frame metadata analyzers publish (`ebur128`, `astats`, `signalstats`, `scdet`, `cropdetect`, ...) as a list of `[key, value]` |

Audio-only graphs run in the audio thread, tick-exact. Graphs with video pads
run on the render thread as a gfx node whose frame transport depends on the
backend: on Vulkan the frames stay on the GPU (FFmpeg's `*_vulkan` filters,
`libplacebo`, and `*_cuda` on NVIDIA); elsewhere the input is read back,
filtered on the CPU and uploaded through score's GPU video decoders.

## Requirements

FFmpeg >= 6.1 (libavfilter >= 9.7, the segment API). GPU filters need an
FFmpeg built with them: the ossia SDK does since
[ossia/sdk#39](https://github.com/ossia/sdk/pull/39); on a distro FFmpeg run
the headless test to see what is available:

```
$ score_addon_lavfi_graph_test
lavfi graph tests: ok (0 failures); vulkan filters: yes, cuda: yes, libplacebo: yes
```

## Layout

```
Lavfi/Core/Graph.*     Qt-free libavfilter wrapper: describe, init, push/pull, commands, log capture
Lavfi/Process.*        the score process: script property, ports derived from the graph
Lavfi/Layer.hpp        script editor dialog
Lavfi/Executor.*       picks the execution backend
Lavfi/AudioNode.*      ossia node for audio graphs
Lavfi/Node.*           gfx node + renderer for video graphs (CPU transport today)
tests/GraphTest.cpp    headless tests of the wrapper
```

Build like any score addon: clone into `score/src/addons/` for an in-tree
developer build, or build against the SDK with `-DSCORE_SDK=...`
(`.github/workflows/builds.yaml` does both).

License: GPLv3, like score.
