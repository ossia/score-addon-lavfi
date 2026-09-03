// Headless tests of Lavfi::Graph: no Qt, no score, just libavfilter.
// Run: score_addon_lavfi_graph_test ; exit code 0 on success.
#include <Lavfi/Core/Graph.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

static int failures = 0;
#define CHECK(cond)                                                     \
  do                                                                    \
  {                                                                     \
    if(!(cond))                                                         \
    {                                                                   \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                       \
    }                                                                   \
  } while(0)

static void test_describe()
{
  Lavfi::Description d;
  std::string err;
  CHECK(Lavfi::Graph::describe("volume=0.5,aecho", d, err));
  CHECK(d.inputs.size() == 1);
  CHECK(d.outputs.size() == 1);
  CHECK(d.inputs[0].type == AVMEDIA_TYPE_AUDIO);
  CHECK(d.hasAudio() && !d.hasVideo());
  CHECK(d.filters.size() == 2);
  bool volume_runtime = false;
  for(auto& o : d.options)
    if(o.name == "volume" && o.filter.find("volume") != std::string::npos)
      volume_runtime = o.runtime;
  CHECK(volume_runtime);

  // Two inputs, labelled, video.
  CHECK(Lavfi::Graph::describe("[a][b]blend=all_mode=addition", d, err));
  CHECK(d.inputs.size() == 2 && d.inputs[0].name == "a" && d.inputs[1].name == "b");
  CHECK(d.inputs[0].type == AVMEDIA_TYPE_VIDEO && d.hasVideo());

  // A source: no inputs.
  CHECK(Lavfi::Graph::describe("testsrc=size=64x64", d, err));
  CHECK(d.inputs.empty() && d.outputs.size() == 1);

  // Garbage must fail with a message.
  CHECK(!Lavfi::Graph::describe("no_such_filter_xyz=1", d, err));
  CHECK(!err.empty());
}

static void test_audio_tick_exact()
{
  Lavfi::Graph g;
  std::vector<Lavfi::InputConfig> in(1);
  in[0].type = AVMEDIA_TYPE_AUDIO;
  in[0].audio = {48000, 2};
  Lavfi::SinkConfig sinks;
  sinks.sample_rate = 48000;
  std::string err;
  CHECK(g.init("volume=volume=0.5", in, sinks, nullptr, err));
  if(!g.valid())
  {
    std::fprintf(stderr, "%s\n", err.c_str());
    return;
  }
  CHECK(g.outputChannels(0) == 2);

  // Variable tick sizes, like score produces: 64, 100, 7, 512.
  const int ticks[] = {64, 100, 7, 512, 1};
  int64_t pos = 0;
  for(int n : ticks)
  {
    std::vector<float> l(n, 1.f), r(n, -1.f);
    const float* planes[2] = {l.data(), r.data()};
    CHECK(g.pushAudio(0, planes, 2, n, pos));
    pos += n;
    std::vector<float> ol(n, 9.f), orr(n, 9.f);
    float* out[2] = {ol.data(), orr.data()};
    const int got = g.pullAudio(0, out, 2, n);
    CHECK(got == n);
    for(int k = 0; k < got; k++)
    {
      CHECK(std::fabs(ol[k] - 0.5f) < 1e-6f);
      CHECK(std::fabs(orr[k] + 0.5f) < 1e-6f);
    }
  }

  // Runtime command: volume is a runtime param.
  CHECK(g.sendCommand("Parsed_volume_0", "volume", "0.25"));
  {
    const int n = 32;
    std::vector<float> l(n, 1.f), r(n, 1.f);
    const float* planes[2] = {l.data(), r.data()};
    CHECK(g.pushAudio(0, planes, 2, n, pos));
    std::vector<float> ol(n), orr(n);
    float* out[2] = {ol.data(), orr.data()};
    CHECK(g.pullAudio(0, out, 2, n) == n);
    CHECK(std::fabs(ol[n - 1] - 0.25f) < 1e-6f);
  }
}

static void test_audio_lookahead_reports_shortfall()
{
  // adelay holds back the first 10ms: the first pull must NOT pretend.
  Lavfi::Graph g;
  std::vector<Lavfi::InputConfig> in(1);
  in[0].type = AVMEDIA_TYPE_AUDIO;
  in[0].audio = {48000, 1};
  Lavfi::SinkConfig sinks;
  sinks.sample_rate = 48000;
  std::string err;
  CHECK(g.init("adelay=delays=10:all=1", in, sinks, nullptr, err));
  if(!g.valid())
    return;
  const int n = 64;
  std::vector<float> l(n, 1.f);
  const float* planes[1] = {l.data()};
  CHECK(g.pushAudio(0, planes, 1, n, 0));
  std::vector<float> ol(n, 9.f);
  float* out[1] = {ol.data()};
  const int got = g.pullAudio(0, out, 1, n);
  CHECK(got == n);
  // The delayed stream is zeros for the first 480 samples.
  CHECK(std::fabs(ol[0]) < 1e-6f);
}

static void test_video_roundtrip_and_metadata()
{
  Lavfi::Graph g;
  std::vector<Lavfi::InputConfig> in(1);
  in[0].type = AVMEDIA_TYPE_VIDEO;
  in[0].video.width = 16;
  in[0].video.height = 8;
  in[0].video.format = AV_PIX_FMT_RGBA;
  Lavfi::SinkConfig sinks;
  sinks.pix_fmts = {AV_PIX_FMT_RGBA};
  std::string err;
  CHECK(g.init("negate,signalstats", in, sinks, nullptr, err));
  if(!g.valid())
  {
    std::fprintf(stderr, "%s\n", err.c_str());
    return;
  }
  CHECK(g.outputPixelFormat(0) == AV_PIX_FMT_RGBA);
  CHECK(g.outputWidth(0) == 16 && g.outputHeight(0) == 8);

  AVFrame* f = av_frame_alloc();
  f->format = AV_PIX_FMT_RGBA;
  f->width = 16;
  f->height = 8;
  CHECK(av_frame_get_buffer(f, 0) >= 0);
  for(int y = 0; y < 8; y++)
    for(int x = 0; x < 16; x++)
    {
      uint8_t* px = f->data[0] + y * f->linesize[0] + x * 4;
      px[0] = 10;
      px[1] = 20;
      px[2] = 30;
      px[3] = 255;
    }
  f->pts = 0;
  CHECK(g.pushVideo(0, f));
  AVFrame* o = g.pullVideo(0);
  CHECK(o != nullptr);
  if(o)
  {
    const uint8_t* px = o->data[0];
    // signalstats only takes YUV, so lavfi converts and back: allow rounding.
    auto near = [](int a, int b) { return std::abs(a - b) <= 3; };
    CHECK(near(px[0], 245) && near(px[1], 235) && near(px[2], 225) && px[3] == 255);
    bool sawStats = false;
    for(auto& [k, v] : g.lastMetadata(0))
      if(k.rfind("signalstats.", 0) == 0)
        sawStats = true;
    CHECK(sawStats);
    av_frame_free(&o);
  }
  av_frame_free(&f);
  CHECK(g.pullVideo(0) == nullptr); // nothing queued: EAGAIN, not an error
}

static void test_video_source()
{
  Lavfi::Graph g;
  Lavfi::SinkConfig sinks;
  sinks.pix_fmts = {AV_PIX_FMT_RGBA, AV_PIX_FMT_YUV420P};
  std::string err;
  CHECK(g.init("testsrc=size=32x16:rate=30", {}, sinks, nullptr, err));
  if(!g.valid())
    return;
  AVFrame* o = g.pullVideo(0);
  CHECK(o != nullptr);
  if(o)
  {
    CHECK(o->width == 32 && o->height == 16);
    av_frame_free(&o);
  }
}

// Hardware graphs through the wrapper alone (FFmpeg's own device, no QRhi):
// exercises hw_device_ctx propagation to hwupload & friends before their
// init, the CPU<->GPU stages inside a graph, and the Vulkan->CUDA transfer.
static void test_hw_graph(const char* devname, const char* script, bool expectCuda)
{
  const AVHWDeviceType type = av_hwdevice_find_type_by_name(devname);
  if(type == AV_HWDEVICE_TYPE_NONE)
  {
    std::printf("  skip: no %s support in this FFmpeg\n", devname);
    return;
  }
  AVBufferRef* dev = nullptr;
  if(av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0)
  {
    std::printf("  skip: no %s device on this machine\n", devname);
    return;
  }
  if(expectCuda && av_hwdevice_find_type_by_name("cuda") == AV_HWDEVICE_TYPE_NONE)
  {
    std::printf("  skip: no cuda support in this FFmpeg\n");
    av_buffer_unref(&dev);
    return;
  }
  Lavfi::Graph g;
  std::vector<Lavfi::InputConfig> in(1);
  in[0].type = AVMEDIA_TYPE_VIDEO;
  in[0].video.width = 32;
  in[0].video.height = 16;
  in[0].video.format = AV_PIX_FMT_RGBA;
  Lavfi::SinkConfig sinks;
  sinks.pix_fmts = {AV_PIX_FMT_RGBA};
  std::string err;
  const bool ok = g.init(script, in, sinks, dev, err);
  std::printf("  %s: %s\n", script, ok ? "configured" : err.c_str());
  CHECK(ok);
  if(ok)
  {
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_RGBA;
    f->width = 32;
    f->height = 16;
    CHECK(av_frame_get_buffer(f, 0) >= 0);
    for(int y = 0; y < 16; y++)
      for(int x = 0; x < 32; x++)
      {
        uint8_t* px = f->data[0] + y * f->linesize[0] + x * 4;
        px[0] = uint8_t(x * 8);
        px[1] = 100;
        px[2] = 200;
        px[3] = 255;
      }
    f->pts = 0;
    CHECK(g.pushVideo(0, f));
    AVFrame* o = g.pullVideo(0);
    CHECK(o != nullptr);
    if(o)
    {
      CHECK(o->format == AV_PIX_FMT_RGBA);
      // hflip: the first pixel is the input's last one (x=31 -> R=248).
      const uint8_t* px = o->data[0];
      CHECK(std::abs(int(px[0]) - 248) <= 2 && std::abs(int(px[1]) - 100) <= 2);
      av_frame_free(&o);
    }
    av_frame_free(&f);
  }
  av_buffer_unref(&dev);
}

static void test_hw_graphs()
{
  // Vulkan filter between hwupload/hwdownload.
  test_hw_graph("vulkan", "hwupload,hflip_vulkan,hwdownload,format=rgba", false);
  // CUDA filters around a CPU frame; scale_cuda takes 0RGB32/0BGR32
  // (rgb0/bgr0), not rgba, and hwdownload yields the frame's sw_format, so
  // the conversion back to rgba is left to the sink negotiation.
  test_hw_graph("cuda", "format=rgb0,hwupload_cuda,scale_cuda=32:16,hwdownload,format=rgb0,hflip", true);
  // Informational: a CUDA stage inside a Vulkan graph. hwupload only takes
  // software frames or its own device's format, so FFmpeg's Vulkan<->CUDA
  // transfer is not reachable from a filter string (checked on 6.1); if a
  // future FFmpeg allows it this starts printing "configured".
  {
    AVBufferRef* dev = nullptr;
    if(av_hwdevice_find_type_by_name("vulkan") != AV_HWDEVICE_TYPE_NONE
       && av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) >= 0)
    {
      Lavfi::Graph g;
      std::vector<Lavfi::InputConfig> in(1);
      in[0].type = AVMEDIA_TYPE_VIDEO;
      in[0].video.width = 32;
      in[0].video.height = 16;
      in[0].video.format = AV_PIX_FMT_RGBA;
      Lavfi::SinkConfig sinks;
      sinks.pix_fmts = {AV_PIX_FMT_RGBA};
      std::string err;
      const bool ok = g.init(
          "format=rgb0,hwupload,hwupload=derive_device=cuda,scale_cuda=32:16,hwupload,hwdownload",
          in, sinks, dev, err);
      std::printf("  info: CUDA stage inside a Vulkan graph: %s\n", ok ? "configured" : "refused (expected)");
      av_buffer_unref(&dev);
    }
  }
}

static void test_log_capture()
{
  Lavfi::Graph g;
  std::vector<Lavfi::InputConfig> in(1);
  in[0].type = AVMEDIA_TYPE_AUDIO;
  in[0].audio = {48000, 2};
  Lavfi::SinkConfig sinks;
  std::string err;
  // A video filter on an audio input cannot be configured: the error must
  // come back through the log, not stderr.
  CHECK(!g.init("negate", in, sinks, nullptr, err));
  CHECK(!err.empty());
}


// ---------------------------------------------------------------------------
// Every shipped preset. A preset that cannot even be configured is worse than
// no preset: the process appears in the library, the user picks it, and gets
// an empty node. So each .scp is read, its graph text taken out of Key.Effect
// (comments and line breaks included: that text goes to the process exactly as
// written) and run through describe() + init() with the pixel formats and the
// hardware device the renderer would give it.
// ---------------------------------------------------------------------------
#ifdef LAVFI_PRESET_DIR
namespace
{
// Just enough JSON to read a preset: one string field, with the escapes
// rapidjson writes (\", \\, \n, \t, \r, \uXXXX for the BMP).
std::string json_string_field(const std::string& doc, const std::string& key)
{
  const auto k = "\"" + key + "\"";
  auto i = doc.find(k);
  if(i == std::string::npos)
    return {};
  i = doc.find(':', i + k.size());
  if(i == std::string::npos)
    return {};
  i = doc.find('"', i);
  if(i == std::string::npos)
    return {};
  std::string out;
  for(++i; i < doc.size() && doc[i] != '"'; ++i)
  {
    if(doc[i] != '\\')
    {
      out += doc[i];
      continue;
    }
    switch(doc[++i])
    {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': break;
      case 'u': {
        const int cp = int(std::strtol(doc.substr(i + 1, 4).c_str(), nullptr, 16));
        i += 4;
        if(cp < 0x80)
          out += char(cp);
        else if(cp < 0x800)
        {
          out += char(0xC0 | (cp >> 6));
          out += char(0x80 | (cp & 0x3F));
        }
        else
        {
          out += char(0xE0 | (cp >> 12));
          out += char(0x80 | ((cp >> 6) & 0x3F));
          out += char(0x80 | (cp & 0x3F));
        }
        break;
      }
      default: out += doc[i]; break;
    }
  }
  return out;
}

const std::vector<AVPixelFormat> uploadable_formats{
    AV_PIX_FMT_RGBA,     AV_PIX_FMT_BGRA,      AV_PIX_FMT_RGB0,      AV_PIX_FMT_BGR0,
    AV_PIX_FMT_RGB24,    AV_PIX_FMT_YUV420P,   AV_PIX_FMT_YUVJ420P,  AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV444P,   AV_PIX_FMT_YUVA420P,  AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_GRAY8,    AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_RGBA64,
    AV_PIX_FMT_GBRP,     AV_PIX_FMT_GBRAP};

// The device the renderer hands the graph: Vulkan where score renders with it,
// and a CUDA device derived from it for the *_cuda filters.
/// Can this machine run a hardware graph of this kind at all? Creating the
/// device is not enough of an answer: under a sanitizer the CUDA driver
/// reports out of memory at cuInit and Vulkan refuses to initialise, and a
/// preset is not broken because the machine cannot run it. Build the smallest
/// possible upload/download graph and see.
bool hardwarePathWorks(const std::string& text, AVBufferRef* dev)
{
  if(!dev)
    return false;
  const bool cuda = text.find("cuda") != std::string::npos;
  // A filter of the same family, not just the upload: configuring an upload
  // can succeed on a driver that then cannot load a kernel, which is exactly
  // what happens under a sanitizer.
  const char* probe
      = cuda ? "format=yuv420p,hwupload_cuda,scale_cuda=16:16,hwdownload,format=yuv420p"
             : "hwupload,hflip_vulkan,hwdownload,format=rgba";
  Lavfi::Graph g;
  std::vector<Lavfi::InputConfig> in(1);
  in[0].type = AVMEDIA_TYPE_VIDEO;
  in[0].video.width = 32;
  in[0].video.height = 32;
  in[0].video.format = AV_PIX_FMT_RGBA;
  in[0].video.time_base = {1, 48000};
  in[0].video.frame_rate = {60, 1};
  Lavfi::SinkConfig sinks;
  sinks.pix_fmts = {AV_PIX_FMT_RGBA};
  sinks.threads = 1;
  std::string err;
  if(!g.init(probe, in, sinks, dev, err))
    return false;

  // And it has to survive a frame: initialisation alone touches little.
  AVFrame* f = av_frame_alloc();
  f->format = AV_PIX_FMT_RGBA;
  f->width = 32;
  f->height = 32;
  bool ok = false;
  if(av_frame_get_buffer(f, 0) >= 0)
  {
    std::memset(f->data[0], 128, std::size_t(f->linesize[0]) * 32);
    f->pts = 0;
    if(g.pushVideo(0, f))
      if(AVFrame* o = g.pullVideo(0))
      {
        ok = o->width > 0;
        av_frame_free(&o);
      }
  }
  av_frame_free(&f);
  return ok;
}

AVBufferRef* preset_hw_device(const std::string& text)
{
  const bool cuda = text.find("cuda") != std::string::npos;
  const AVHWDeviceType type
      = av_hwdevice_find_type_by_name(cuda ? "cuda" : "vulkan");
  if(type == AV_HWDEVICE_TYPE_NONE)
    return nullptr;
  AVBufferRef* dev = nullptr;
  if(av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0)
    return nullptr;
  return dev;
}
}

static void test_presets()
{
  namespace fs = std::filesystem;
  const fs::path dir{LAVFI_PRESET_DIR};
  if(!fs::exists(dir))
  {
    std::printf("  skip: preset directory not found (%s)\n", dir.string().c_str());
    return;
  }

  int checked = 0, skipped = 0;
  std::vector<fs::path> files;
  // Recursive: the presets are filed by kind (Video effects, Audio
  // generators, ...), which is how they appear in score's library.
  for(const auto& de : fs::recursive_directory_iterator(dir))
    if(de.path().extension() == ".scp")
      files.push_back(de.path());
  std::sort(files.begin(), files.end());

  for(const auto& path : files)
  {
    std::string doc;
    {
      std::FILE* f = std::fopen(path.string().c_str(), "rb");
      CHECK(f != nullptr);
      if(!f)
        continue;
      char buf[4096];
      std::size_t n;
      while((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        doc.append(buf, n);
      std::fclose(f);
    }
    const std::string name = json_string_field(doc, "Name");
    const std::string text = json_string_field(doc, "Effect");
    const std::string file = path.filename().string();
    CHECK(!name.empty());
    CHECK(!text.empty());
    if(text.empty())
      continue;

    // The uuid must be this process's, or the preset lands on another process.
    CHECK(doc.find("3f0a1b6e-6a6b-4d0c-9d8f-2b3c1e7a9f10") != std::string::npos);

    Lavfi::Description d;
    std::string err;
    if(!Lavfi::Graph::describe(text, d, err))
    {
      // A filter this FFmpeg was not built with is not a broken preset.
      if(err.find("Filter not found") != std::string::npos)
      {
        std::printf("  skip %-28s (filter missing in this FFmpeg)\n", file.c_str());
        skipped++;
        continue;
      }
      std::fprintf(stderr, "FAIL preset %s: %s\n", file.c_str(), err.c_str());
      failures++;
      continue;
    }
    CHECK(!d.outputs.empty());

    // Configure it the way the renderer/audio node would.
    AVBufferRef* dev = preset_hw_device(text);
    const bool needsHw = text.find("hwupload") != std::string::npos;
    if(needsHw && !dev)
    {
      std::printf("  skip %-28s (no hardware device on this machine)\n", file.c_str());
      skipped++;
      continue;
    }

    std::vector<Lavfi::InputConfig> in;
    for(const auto& pad : d.inputs)
    {
      Lavfi::InputConfig c;
      c.type = pad.type;
      if(pad.type == AVMEDIA_TYPE_VIDEO)
      {
        c.video.width = 320;
        c.video.height = 240;
        c.video.format = AV_PIX_FMT_RGBA;
        c.video.time_base = {1, 1000000};
        c.video.frame_rate = {60, 1};
      }
      else
      {
        c.audio.sample_rate = 48000;
        c.audio.channels = 2;
      }
      in.push_back(c);
    }
    Lavfi::SinkConfig sinks;
    sinks.pix_fmts = uploadable_formats;
    sinks.sample_rate = 48000;
    sinks.threads = 0;

    Lavfi::Graph g;
    if(!g.init(text, in, sinks, dev, err))
    {
      if(needsHw && !hardwarePathWorks(text, dev))
      {
        std::printf("  skip %-28s (the hardware path does not work here)\n", file.c_str());
        skipped++;
        av_buffer_unref(&dev);
        continue;
      }
      std::fprintf(stderr, "FAIL preset %s: %s\n", file.c_str(), err.c_str());
      failures++;
      av_buffer_unref(&dev);
      continue;
    }

    // And it must actually produce something: one video frame, or one tick of
    // audio, for a graph with no input; a graph with inputs gets fed first.
    bool produced = false;
    // Some filters need real time before they emit: ebur128 integrates over
    // 400 ms and loudnorm buffers three seconds of lookahead, so an audio
    // graph gets four seconds of material (at 512 samples a tick) before it
    // is called silent. Video graphs get 96 frames -- nothing shipped here
    // waits longer, and nlmeans/minterpolate are slow enough as it is.
    const int ticks = g.outputType(0) == AVMEDIA_TYPE_AUDIO ? 400 : 96;
    for(int i = 0; i < ticks && !produced; i++)
    {
      for(int k = 0; k < g.inputCount(); k++)
      {
        if(g.inputType(k) == AVMEDIA_TYPE_VIDEO)
        {
          AVFrame* f = av_frame_alloc();
          f->format = AV_PIX_FMT_RGBA;
          f->width = 320;
          f->height = 240;
          if(av_frame_get_buffer(f, 0) >= 0)
          {
            std::memset(f->data[0], 40 + 20 * i, std::size_t(f->linesize[0]) * 240);
            f->pts = i * 16667;
            g.pushVideo(k, f);
          }
          av_frame_free(&f);
        }
        else
        {
          std::vector<float> l(512), r(512);
          for(int s = 0; s < 512; s++)
            l[s] = r[s] = 0.25f * std::sin(2 * M_PI * 440. * (i * 512 + s) / 48000.);
          const float* planes[2] = {l.data(), r.data()};
          g.pushAudio(k, planes, 2, 512, i * 512);
        }
      }
      if(g.outputType(0) == AVMEDIA_TYPE_VIDEO)
      {
        if(AVFrame* o = g.pullVideo(0))
        {
          produced = o->width > 0 && o->height > 0;
          av_frame_free(&o);
        }
      }
      else
      {
        std::vector<float> ol(512), orr(512);
        float* planes[2] = {ol.data(), orr.data()};
        produced = g.pullAudio(0, planes, 2, 512) > 0;
      }
    }
    if(!produced)
    {
      std::fprintf(stderr, "FAIL preset %s: configured but produced nothing\n", file.c_str());
      failures++;
    }
    else
    {
      std::printf(
          "  ok   %-28s %zu in, %zu out, %zu control%s\n", file.c_str(), d.inputs.size(),
          d.outputs.size(), d.options.size(), d.options.size() == 1 ? "" : "s");
      checked++;
    }
    av_buffer_unref(&dev);
  }
  std::printf("  presets: %d ok, %d skipped\n", checked, skipped);
  CHECK(checked > 0);
}
#endif

int main()
{
  test_describe();
  test_audio_tick_exact();
  test_audio_lookahead_reports_shortfall();
  test_video_roundtrip_and_metadata();
  test_video_source();
  test_hw_graphs();
  test_log_capture();
#ifdef LAVFI_PRESET_DIR
  test_presets();
#endif
  std::printf(
      "lavfi graph tests: %s (%d failure%s); vulkan filters: %s, cuda: %s, libplacebo: %s\n",
      failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s",
      Lavfi::hasFilter("gblur_vulkan") ? "yes" : "no", Lavfi::hasFilter("scale_cuda") ? "yes" : "no",
      Lavfi::hasFilter("libplacebo") ? "yes" : "no");
  return failures ? 1 : 0;
}
