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
  GPU video decoders (colour conversion on the GPU, 18 pixel formats).
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

`Presets/FFmpeg filter/*.scp` are score presets: video and audio effects,
generators, analysers, and the GPU ones. Copy the `Presets` folder into your
score user library (the one the Library settings point at) and they appear
under the process in the library panel.

Their graph text is written the way a person writes it, over several lines with
`#` comments, and that is also what is put in the script editor when the preset
is used. libavfilter accepts neither, so `Lavfi::Graph::preprocess` strips
whole-line comments and joins the lines before parsing. `#` inside a line is
left alone: that is a colour (`color=#ff0000`).

A `.lavfi` file anywhere in the library is the same thing without the score
preset wrapper: one graph per file, and it can be dropped on a scenario.

Four tests cover them, each answering a different question:

```
$ score_addon_lavfi_graph_test                # does every preset configure and emit?
$ score_addon_lavfi_preset_test               # does it do what it says?
$ score_addon_lavfi_gfx_test                  # does the picture reach the screen?
$ tests/run-presets.sh /path/to/ossia-score   # does score build and run them?
```

The behaviour test feeds each preset material chosen for it and checks the
values that come back: the negated picture is the inverse of the input, the
8 kHz tone is gone after the 4 kHz low-pass, the pixelator leaves flat 16x16
blocks, the loudness meter reports the level it was fed. The gfx test renders
source shader to lavfi node to an offscreen sink on every backend the machine
has and checks the read-back pixels. The script test creates each preset's
process in a real document through `--script`, checks the ports it built, and
plays all of them at once.

A preset whose filters the local FFmpeg was not built with (`v360_vulkan`
before FFmpeg 7) is reported as skipped, not failed.

One thing worth knowing when writing a graph for the Vulkan transport: a filter
that only rewrites strides rather than reading pixels, `vflip` being the one
that bites, will happily accept a hardware frame and leave it untouched, since
what it flips is a pointer to a `VkImage`. Anything that has to look at the
pixels makes libavfilter insert the download and upload it needs.

## Layout

```
Lavfi/Core/Graph.*          Qt-free libavfilter wrapper: describe, init, push/pull, commands, log capture
Lavfi/Process.*             the score process: script property, ports derived from the graph
Lavfi/Layer.hpp             script editor dialog
Lavfi/Commands.hpp          undoable graph edit (cables of dropped ports are restored)
Lavfi/Library.hpp           .lavfi files in the library, drag and drop
Lavfi/Executor.*            picks the execution backend, live reload
Lavfi/AudioNode.*           ossia node for audio graphs
Lavfi/Node.*                gfx node + renderer for video graphs, transport ladder
Lavfi/Gfx/HwDevice.*        FFmpeg device contexts: Vulkan over QRhi (tested), CUDA (tested), D3D11 / VideoToolbox (untested here)
Lavfi/Gfx/VulkanTransport.* the zero-copy transport
tests/GraphTest.cpp         headless tests (CPU, Vulkan and CUDA graphs through the wrapper)
tests/VulkanTransportTest.cpp GPU test: pool image <-> QRhi <-> hflip_vulkan, pixel-checked
tests/presets.js            every preset through score itself (--script), tests/run-presets.sh runs it
Presets/FFmpeg filter/*.scp the shipped presets
```

Build like any score addon: clone into `score/src/addons/` for an in-tree
developer build, or build against the SDK with `-DSCORE_SDK=...`
(`.github/workflows/builds.yaml` does both). `-DSCORE_LAVFI_GPU_TESTS=ON`
adds the Vulkan test.

License: GPLv3, like score.
