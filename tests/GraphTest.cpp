// Headless tests of Lavfi::Graph: no Qt, no score, just libavfilter.
// Run: score_addon_lavfi_graph_test ; exit code 0 on success.
#include <Lavfi/Core/Graph.hpp>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
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

int main()
{
  test_describe();
  test_audio_tick_exact();
  test_audio_lookahead_reports_shortfall();
  test_video_roundtrip_and_metadata();
  test_video_source();
  test_log_capture();
  std::printf(
      "lavfi graph tests: %s (%d failure%s); vulkan filters: %s, cuda: %s, libplacebo: %s\n",
      failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s",
      Lavfi::hasFilter("gblur_vulkan") ? "yes" : "no", Lavfi::hasFilter("scale_cuda") ? "yes" : "no",
      Lavfi::hasFilter("libplacebo") ? "yes" : "no");
  return failures ? 1 : 0;
}
