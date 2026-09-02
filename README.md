# score-addon-lavfi

FFmpeg [libavfilter](https://ffmpeg.org/ffmpeg-filters.html) graphs as
[ossia score](https://ossia.io) processes.

One process, **FFmpeg filter**, whose program is a filtergraph string in the
same syntax as `ffmpeg -filter_complex`:

```
gblur_vulkan=sigma=4                # GPU, zero-copy on the Vulkan backend
gblur=sigma=4,hue=h=90              # CPU, any backend
[a][b]xfade_vulkan=transition=fade  # two inputs
showspectrum=s=1024x512             # audio in, video out
loudnorm=I=-16                      # audio
testsrc2=size=1280x720:rate=60      # generator, no input
```

The process's ports are derived from the graph:

| port | from |
|---|---|
| audio / texture inlets | the graph's open input pads |
| audio / texture outlets | the graph's open output pads |
| control inlets `filter/option` | every option flagged runtime-changeable on any filter instance, forwarded with `avfilter_process_command` |
| `Metadata` value outlet | the `lavfi.*` frame metadata analyzers publish (`ebur128`, `astats`, `signalstats`, `scdet`, `cropdetect`, ...) as a list of `[key, value]` |

Editing the graph while playing swaps the program live; when the port
layout changes the execution node is replaced under the running graph.

## How frames move

Audio-only graphs run in the audio thread, tick-exact
(`av_buffersink_get_samples`). Graphs with video pads run on the render thread
as a gfx node with one of two transports, picked per node at init
(`SCORE_LAVFI_TRANSPORT=auto|vulkan|cpu`, `SCORE_LAVFI_DEBUG=1` logs the choice):

- **Vulkan**, on the Vulkan backend: score creates its VkDevice itself with
  every feature the GPU reports, and the addon builds FFmpeg's device context
  over it. The input render targets *are* FFmpeg pool images, so the upstream
  node draws into them, and the sink frame is sampled directly. No readback,
  no upload, no host wait; ordering uses the frames' own timeline semaphores.
  Works with `*_vulkan` filters and `libplacebo`; a graph that refuses Vulkan
  frames (a CPU filter) falls back to the CPU transport automatically. FFmpeg
  6.1 additionally wants `VK_EXT_descriptor_buffer` on the device, which
  score's shared device does not enable, so on a 6.1 developer build the
  Vulkan transport is only reachable from the GPU test; FFmpeg 7.1+ (the SDK
  ships 9.0) has no such requirement.
- **CPU**, everywhere else: the input is read back asynchronously (one frame
  of latency, no GPU stall), filtered, and the result uploaded through score's
  GPU video decoders (colour conversion on the GPU, any of ~20 pixel formats).
  The graph still gets a hardware device, so GPU stages inside the string work
  on every backend: `format=rgb0,hwupload_cuda,scale_cuda=...,hwdownload` on
  NVIDIA, `hwupload,scale_d3d11=...,hwdownload` on D3D11, `hwupload,scale_vt=...,
  hwdownload` on macOS.

Not possible with FFmpeg's filter set: a CUDA stage *inside* a Vulkan graph
(`hwupload` only takes software frames or its own device's format, so the
Vulkan<->CUDA transfer FFmpeg has is unreachable from a filter string).

## Requirements

FFmpeg >= 6.1 (libavfilter >= 9.7, the segment API). GPU filters need an
FFmpeg built with them: the ossia SDK does since
[ossia/sdk#39](https://github.com/ossia/sdk/pull/39); Ubuntu 24.04's 6.1 does
too. The headless test reports what is available:

```
$ score_addon_lavfi_graph_test
lavfi graph tests: ok (0 failures); vulkan filters: yes, cuda: yes, libplacebo: yes
```

## Presets

`presets/*.lavfi` are graph strings with `#` comments. Copy the folder into
your score user library: each file becomes a preset of the process, and
`.lavfi` files can be dropped on a scenario.

## Layout

```
Lavfi/Core/Graph.*          Qt-free libavfilter wrapper: describe, init, push/pull, commands, log capture
Lavfi/Process.*             the score process: script property, ports derived from the graph
Lavfi/Layer.hpp             script editor dialog
Lavfi/Commands.hpp          undoable graph edit (cables of dropped ports are restored)
Lavfi/Library.hpp           .lavfi presets in the library, drag and drop
Lavfi/Executor.*            picks the execution backend, live reload
Lavfi/AudioNode.*           ossia node for audio graphs
Lavfi/Node.*                gfx node + renderer for video graphs, transport ladder
Lavfi/Gfx/HwDevice.*        FFmpeg device contexts over QRhi (Vulkan, D3D11), CUDA, VideoToolbox
Lavfi/Gfx/VulkanTransport.* the zero-copy transport
tests/GraphTest.cpp         headless tests (CPU, Vulkan and CUDA graphs through the wrapper)
tests/VulkanTransportTest.cpp GPU test: pool image <-> QRhi <-> hflip_vulkan, byte-exact
```

Build like any score addon: clone into `score/src/addons/` for an in-tree
developer build, or build against the SDK with `-DSCORE_SDK=...`
(`.github/workflows/builds.yaml` does both). `-DSCORE_LAVFI_GPU_TESTS=ON`
adds the Vulkan test.

License: GPLv3, like score.
