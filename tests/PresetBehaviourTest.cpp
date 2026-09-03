// What every shipped preset actually DOES.
//
// tests/GraphTest.cpp checks that a preset configures and emits something;
// tests/presets.js checks that score builds a process and ports out of it and
// can run them. Neither looks at the result. This one does: each preset gets
// an input chosen for it, the output frames or sample buffers come back, and
// the assertion is about the values -- the negated image really is the
// inverse, the low-passed 8 kHz tone really is gone, the loudness meter
// really reports the level of what it was fed.
//
// No Qt, no score, no GPU beyond what the hardware presets ask for.
// Run: score_addon_lavfi_preset_test ; exit code 0 on success.
#include <Lavfi/Core/Graph.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <numbers>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace
{
int failures = 0;
std::string current;

void fail(const std::string& what)
{
  std::fprintf(stderr, "FAIL %s: %s\n", current.c_str(), what.c_str());
  failures++;
}

#define EXPECT(cond, what)  \
  do                        \
  {                         \
    if(!(cond))             \
      fail(what);           \
  } while(0)

// ---------------------------------------------------------------------------
//  Reading a preset file
// ---------------------------------------------------------------------------
std::string readFile(const std::filesystem::path& p)
{
  std::string doc;
  std::FILE* f = std::fopen(p.string().c_str(), "rb");
  if(!f)
    return doc;
  char buf[4096];
  std::size_t n;
  while((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    doc.append(buf, n);
  std::fclose(f);
  return doc;
}

/// The one string field a preset test needs, with the escapes rapidjson writes.
std::string jsonString(const std::string& doc, const std::string& key)
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
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case 'r':
        break;
      default:
        out += doc[i];
        break;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
//  Input material
// ---------------------------------------------------------------------------
constexpr int VW = 64, VH = 48;
constexpr int SR = 48000, BLK = 512;
constexpr double PI = std::numbers::pi;

enum class VIn
{
  None,
  /// Four vertical bands: pure green, black, white, ramp. A hard black->white
  /// step at x=32 for the blurs and the edge detector, a ramp at x>=48 for the
  /// pixelator, saturated green at x<16 for the keyers.
  Pattern,
  /// One flat mid grey. For anything whose effect is a shading of a flat field.
  Uniform,
  Black,
  /// Flat grey plus deterministic per-pixel noise.
  Noisy,
  /// White centre inside a black border, for the crop detector.
  Bordered,
  /// Black and white frames in turn, for the temporal filters.
  Alternating,
  /// The same frame every time, for the freeze detector.
  Static,
};

enum class AIn
{
  None,
  Silence,
  Sine100,
  Sine200,
  Sine440,
  Sine1k,
  Sine2k,
  Sine8k,
  /// A full-scale 440 Hz tone: peaks at 1.0.
  Loud,
  /// -60 dBFS: below every gate and threshold here.
  Quiet,
  /// White noise, deterministic.
  Noise,
  /// One block of tone, then silence: what is left afterwards is the tail.
  Burst,
  /// L is the tone, R is half of it: there is a real side signal.
  Wide,
};

uint8_t clamp8(int v)
{
  return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/// A deterministic hash, so "noise" is the same on every machine and run.
uint32_t hash(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

void fillVideo(AVFrame* f, VIn kind, int idx)
{
  for(int y = 0; y < VH; y++)
  {
    uint8_t* row = f->data[0] + std::ptrdiff_t(y) * f->linesize[0];
    for(int x = 0; x < VW; x++)
    {
      uint8_t* px = row + x * 4;
      uint8_t r = 0, g = 0, b = 0;
      switch(kind)
      {
        case VIn::Pattern:
          if(x < 16)
          {
            r = 0;
            g = 255;
            b = 0;
          }
          else if(x < 32)
          {
            r = g = b = 0;
          }
          else if(x < 48)
          {
            r = g = b = 255;
          }
          else
          {
            r = g = b = clamp8((x - 48) * 16);
          }
          break;
        case VIn::Uniform:
        case VIn::Static:
          r = g = b = 128;
          break;
        case VIn::Black:
          r = g = b = 0;
          break;
        case VIn::Noisy: {
          // +-8: sensor-grade noise. Much more than that and an edge-preserving
          // denoiser like hqdn3d is right to leave it alone.
          const int n = int(hash(uint32_t((y * VW + x) * 977 + idx)) % 17) - 8;
          r = g = b = clamp8(128 + n);
          break;
        }
        case VIn::Bordered:
          if(x < 12 || x >= VW - 12 || y < 8 || y >= VH - 8)
            r = g = b = 0;
          else
            r = g = b = 220;
          break;
        case VIn::Alternating:
          r = g = b = (idx % 2) ? 255 : 0;
          break;
        default:
          break;
      }
      px[0] = r;
      px[1] = g;
      px[2] = b;
      px[3] = 255;
    }
  }
}

float sample(AIn kind, int64_t n, int channel)
{
  const double t = double(n) / SR;
  const auto tone = [&](double hz, double amp) {
    return float(amp * std::sin(2 * PI * hz * t));
  };
  switch(kind)
  {
    case AIn::Silence:
      return 0.f;
    case AIn::Sine100:
      return tone(100, 0.5);
    case AIn::Sine200:
      return tone(200, 0.5);
    case AIn::Sine440:
      return tone(440, 0.5);
    case AIn::Sine1k:
      return tone(1000, 0.5);
    case AIn::Sine2k:
      return tone(2000, 0.5);
    case AIn::Sine8k:
      return tone(8000, 0.5);
    case AIn::Loud:
      return tone(440, 1.0);
    case AIn::Quiet:
      return tone(440, 0.001);
    case AIn::Noise:
      return float(int(hash(uint32_t(n) * 2654435761u + uint32_t(channel)) % 20001) - 10000)
             / 20000.f;
    case AIn::Burst:
      return n < BLK ? tone(440, 0.5) : 0.f;
    case AIn::Wide:
      return channel == 0 ? tone(440, 0.5) : tone(440, 0.25);
    default:
      return 0.f;
  }
}

// ---------------------------------------------------------------------------
//  Running one preset
// ---------------------------------------------------------------------------
struct Img
{
  int w{}, h{};
  std::vector<uint8_t> px; // RGBA, tightly packed

  const uint8_t* at(int x, int y) const { return px.data() + (std::size_t(y) * w + x) * 4; }
  double luma(int x, int y) const
  {
    const uint8_t* p = at(x, y);
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
  }
  double meanLuma() const
  {
    double s = 0;
    for(int y = 0; y < h; y++)
      for(int x = 0; x < w; x++)
        s += luma(x, y);
    return s / (w * h);
  }
  double varianceLuma() const
  {
    const double m = meanLuma();
    double s = 0;
    for(int y = 0; y < h; y++)
      for(int x = 0; x < w; x++)
        s += (luma(x, y) - m) * (luma(x, y) - m);
    return s / (w * h);
  }
};

struct Result
{
  bool configured{};
  bool skipped{};
  std::string error;
  std::vector<Img> video;
  /// One vector per channel, per audio output.
  std::vector<std::vector<std::vector<float>>> audio;
  int rate{}, channels{};
  /// Every key any output frame carried, with its last value. A detector
  /// reports once, on the frame where it fires, so keeping only the last
  /// frame's metadata loses exactly what is being tested.
  std::vector<std::pair<std::string, std::string>> meta;
};

void mergeMeta(Result& r, const std::vector<std::pair<std::string, std::string>>& in)
{
  // Appended, not merged: scdet reports its score on the one frame where the
  // scene changes and zero on the others, so the last value is not the answer.
  for(const auto& kv : in)
    if(r.meta.size() < 4096)
      r.meta.push_back(kv);
}

struct Job
{
  std::string text;
  VIn vin{VIn::None};
  std::vector<AIn> ain;  ///< one per audio input pad, repeated if short
  int videoFrames{4};
  int audioBlocks{16};
};

/// Everything hardware-backed needs a device; which one is decided the way the
/// renderer decides it.
AVBufferRef* hwDevice(const std::string& text)
{
  const bool cuda = text.find("cuda") != std::string::npos;
  const AVHWDeviceType type = av_hwdevice_find_type_by_name(cuda ? "cuda" : "vulkan");
  if(type == AV_HWDEVICE_TYPE_NONE)
    return nullptr;
  AVBufferRef* dev = nullptr;
  if(av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0)
    return nullptr;
  return dev;
}

/// Reason this graph cannot be judged on this build, or an empty string.
std::string brokenHere(const std::string& text)
{
#if LIBAVFILTER_VERSION_MAJOR < 10
  // FFmpeg 6.1: gblur_vulkan returns an all-black frame (every other Vulkan
  // filter in these presets is fine, hflip/bwdif/nlmeans/xfade included).
  // Fixed by FFmpeg 7; the SDK ships 9.
  if(text.find("gblur_vulkan") != std::string::npos)
    return "gblur_vulkan returns an empty frame on libavfilter "
           + std::to_string(LIBAVFILTER_VERSION_MAJOR);
#endif
  (void)text;
  return {};
}

bool needsHardware(const std::string& text)
{
  return text.find("hwupload") != std::string::npos;
}

Result run(const Job& job)
{
  Result res;
  Lavfi::Description desc;
  std::string err;
  if(!Lavfi::Graph::describe(job.text, desc, err))
  {
    if(err.find("Filter not found") != std::string::npos)
    {
      res.skipped = true;
      res.error = "a filter this FFmpeg does not have";
      return res;
    }
    res.error = err;
    return res;
  }

  if(auto why = brokenHere(job.text); !why.empty())
  {
    res.skipped = true;
    res.error = why;
    return res;
  }

  AVBufferRef* dev = hwDevice(job.text);
  if(needsHardware(job.text) && !dev)
  {
    res.skipped = true;
    res.error = "no hardware device on this machine";
    return res;
  }

  std::vector<Lavfi::InputConfig> inputs;
  for(const auto& pad : desc.inputs)
  {
    Lavfi::InputConfig c;
    c.type = pad.type;
    if(pad.type == AVMEDIA_TYPE_VIDEO)
    {
      c.video.width = VW;
      c.video.height = VH;
      c.video.format = AV_PIX_FMT_RGBA;
      c.video.time_base = {1, SR};
      c.video.frame_rate = {60, 1};
    }
    else
    {
      c.audio.sample_rate = SR;
      c.audio.channels = 2;
    }
    inputs.push_back(c);
  }

  Lavfi::SinkConfig sinks;
  sinks.pix_fmts = {AV_PIX_FMT_RGBA}; // one format, so the checks read RGBA
  sinks.sample_rate = SR;
  sinks.threads = 1; // deterministic

  Lavfi::Graph g;
  if(!g.init(job.text, inputs, sinks, dev, err))
  {
    res.error = err;
    av_buffer_unref(&dev);
    return res;
  }
  res.configured = true;

  const int outs = g.outputCount();
  res.audio.resize(std::size_t(outs));
  res.channels = g.outputChannels(0);
  res.rate = g.outputSampleRate(0);

  // How many ticks to run: what the INPUT needs. showspectrum and friends take
  // audio and draw pictures, and they need seconds of it before the first
  // frame appears -- counting video frames there would stop far too early.
  bool audioIn = false;
  for(int k = 0; k < g.inputCount(); k++)
    audioIn |= g.inputType(k) == AVMEDIA_TYPE_AUDIO;
  const bool videoOut = g.outputType(0) == AVMEDIA_TYPE_VIDEO;
  const int rounds = (audioIn || !videoOut) ? job.audioBlocks : job.videoFrames;

  AVFrame* in = av_frame_alloc();
  int64_t audioPos = 0;
  for(int i = 0; i < rounds; i++)
  {
    // Feed every input pad.
    int audioPad = 0;
    for(int k = 0; k < g.inputCount(); k++)
    {
      if(g.inputType(k) == AVMEDIA_TYPE_VIDEO)
      {
        av_frame_unref(in);
        in->format = AV_PIX_FMT_RGBA;
        in->width = VW;
        in->height = VH;
        if(av_frame_get_buffer(in, 0) < 0)
          break;
        fillVideo(in, job.vin, i);
        in->pts = int64_t(i) * SR / 60;
        g.pushVideo(k, in);
      }
      else
      {
        const AIn sig = job.ain.empty()
                            ? AIn::Silence
                            : job.ain[std::min<std::size_t>(audioPad, job.ain.size() - 1)];
        audioPad++;
        std::vector<float> l(BLK), r(BLK);
        for(int s = 0; s < BLK; s++)
        {
          l[s] = sample(sig, audioPos + s, 0);
          r[s] = sample(sig, audioPos + s, 1);
        }
        const float* planes[2] = {l.data(), r.data()};
        g.pushAudio(k, planes, 2, BLK, audioPos);
      }
    }
    if(g.inputCount() > 0 && g.inputType(0) == AVMEDIA_TYPE_AUDIO)
      audioPos += BLK;
    else if(g.inputCount() == 0)
      audioPos += BLK;

    // Collect from every output pad.
    for(int o = 0; o < outs; o++)
    {
      if(g.outputType(o) == AVMEDIA_TYPE_VIDEO)
      {
        while(AVFrame* f = g.pullVideo(o))
        {
          if(o == 0 && f->format == AV_PIX_FMT_RGBA)
          {
            Img img;
            img.w = f->width;
            img.h = f->height;
            img.px.resize(std::size_t(img.w) * img.h * 4);
            for(int y = 0; y < img.h; y++)
              std::memcpy(
                  img.px.data() + std::size_t(y) * img.w * 4,
                  f->data[0] + std::ptrdiff_t(y) * f->linesize[0], std::size_t(img.w) * 4);
            res.video.push_back(std::move(img));
            mergeMeta(res, g.lastMetadata(o));
          }
          av_frame_free(&f);
          if(res.video.size() > 512)
            break;
        }
      }
      else
      {
        const int ch = std::max(1, g.outputChannels(o));
        std::vector<std::vector<float>> planes;
        planes.assign(std::size_t(ch), std::vector<float>(BLK));
        std::vector<float*> ptrs;
        for(auto& p : planes)
          ptrs.push_back(p.data());
        const int n = g.pullAudio(o, ptrs.data(), ch, BLK);
        if(n > 0)
        {
          auto& dst = res.audio[std::size_t(o)];
          dst.resize(std::size_t(ch));
          for(int c = 0; c < ch; c++)
            dst[std::size_t(c)].insert(
                dst[std::size_t(c)].end(), planes[std::size_t(c)].begin(),
                planes[std::size_t(c)].begin() + n);
          if(o == 0)
            mergeMeta(res, g.lastMetadata(o));
        }
      }
    }
  }
  av_frame_free(&in);
  av_buffer_unref(&dev);
  return res;
}

// ---------------------------------------------------------------------------
//  Measurements
// ---------------------------------------------------------------------------
double rms(const std::vector<float>& v, std::size_t from = 0, std::size_t to = 0)
{
  if(to == 0 || to > v.size())
    to = v.size();
  if(from >= to)
    return 0;
  double s = 0;
  for(std::size_t i = from; i < to; i++)
    s += double(v[i]) * v[i];
  return std::sqrt(s / double(to - from));
}

double peak(const std::vector<float>& v)
{
  double p = 0;
  for(float x : v)
    p = std::max(p, std::abs(double(x)));
  return p;
}

/// Magnitude of one frequency, normalised like an amplitude (Goertzel).
double toneAt(const std::vector<float>& v, double hz)
{
  if(v.size() < 64)
    return 0;
  const double w = 2 * PI * hz / SR;
  const double c = 2 * std::cos(w);
  double s1 = 0, s2 = 0;
  for(float x : v)
  {
    const double s0 = double(x) + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const double re = s1 - s2 * std::cos(w);
  const double im = s2 * std::sin(w);
  return 2 * std::sqrt(re * re + im * im) / double(v.size());
}

/// Energy between two frequencies, sampled at a few points.
double bandEnergy(const std::vector<float>& v, double lo, double hi)
{
  double s = 0;
  for(int i = 0; i < 8; i++)
  {
    const double f = lo * std::pow(hi / lo, i / 7.0);
    s += toneAt(v, f);
  }
  return s / 8;
}

const std::string* metaValue(const Result& r, const std::string& key)
{
  const std::string* last = nullptr;
  for(const auto& [k, v] : r.meta)
    if(k == key || k.find(key) != std::string::npos)
      last = &v;
  return last;
}

/// The largest value this key ever took, for the detectors that report on one
/// frame and stay quiet on the others.
double metaMax(const Result& r, const std::string& key, double none = -1e30)
{
  double m = none;
  for(const auto& [k, v] : r.meta)
    if(k == key || k.find(key) != std::string::npos)
      m = std::max(m, std::atof(v.c_str()));
  return m;
}

bool hasMeta(const Result& r, const std::string& key)
{
  return metaValue(r, key) != nullptr;
}

std::string metaKeys(const Result& r)
{
  std::string s;
  for(const auto& [k, v] : r.meta)
  {
    if(s.find(k) != std::string::npos)
      continue;
    if(!s.empty())
      s += ", ";
    s += k;
  }
  return s.empty() ? "(none)" : s;
}

/// The input the preset was fed, rebuilt as an image for comparisons.
Img inputImage(VIn kind, int idx)
{
  AVFrame* f = av_frame_alloc();
  f->format = AV_PIX_FMT_RGBA;
  f->width = VW;
  f->height = VH;
  av_frame_get_buffer(f, 0);
  fillVideo(f, kind, idx);
  Img img;
  img.w = VW;
  img.h = VH;
  img.px.resize(std::size_t(VW) * VH * 4);
  for(int y = 0; y < VH; y++)
    std::memcpy(
        img.px.data() + std::size_t(y) * VW * 4, f->data[0] + std::ptrdiff_t(y) * f->linesize[0],
        std::size_t(VW) * 4);
  av_frame_free(&f);
  return img;
}

double maxDiff(const Img& a, const Img& b)
{
  if(a.w != b.w || a.h != b.h)
    return 1e9;
  double d = 0;
  for(int y = 0; y < a.h; y++)
    for(int x = 0; x < a.w; x++)
      for(int c = 0; c < 3; c++)
        d = std::max(d, std::abs(double(a.at(x, y)[c]) - double(b.at(x, y)[c])));
  return d;
}

double meanDiff(const Img& a, const Img& b)
{
  if(a.w != b.w || a.h != b.h)
    return 1e9;
  double d = 0;
  for(int y = 0; y < a.h; y++)
    for(int x = 0; x < a.w; x++)
      for(int c = 0; c < 3; c++)
        d += std::abs(double(a.at(x, y)[c]) - double(b.at(x, y)[c]));
  return d / (a.w * a.h * 3);
}

/// Largest horizontal luma step in a row: how sharp the black->white edge is.
double maxGradient(const Img& img, int y)
{
  double g = 0;
  for(int x = 1; x < img.w; x++)
    g = std::max(g, std::abs(img.luma(x, y) - img.luma(x - 1, y)));
  return g;
}
}

// ---------------------------------------------------------------------------
//  What each preset must do
// ---------------------------------------------------------------------------
namespace
{
using Check = std::function<void(const Result&)>;

struct Case
{
  Job job;
  Check check;
};

// -- shared assertions ------------------------------------------------------
void expectVideo(const Result& r, int frames = 1)
{
  EXPECT(int(r.video.size()) >= frames, "no video frame came out");
}

void expectSize(const Result& r, int w, int h)
{
  if(r.video.empty())
    return;
  EXPECT(
      r.video[0].w == w && r.video[0].h == h,
      "wrong frame size: " + std::to_string(r.video[0].w) + "x"
          + std::to_string(r.video[0].h) + ", expected " + std::to_string(w) + "x"
          + std::to_string(h));
}

/// Not a flat colour: a generator that emits one solid frame is broken.
void expectDetail(const Result& r)
{
  if(r.video.empty())
    return;
  EXPECT(r.video.back().varianceLuma() > 20, "the picture is flat");
}

/// A scope draws thin bright traces on black: the variance of the whole frame
/// stays small, so what says "it drew something" is that some pixels are lit.
void expectLit(const Result& r)
{
  if(r.video.empty())
    return;
  const Img& img = r.video.back();
  double mx = 0;
  int lit = 0;
  for(int y = 0; y < img.h; y++)
    for(int x = 0; x < img.w; x++)
    {
      const double l = img.luma(x, y);
      mx = std::max(mx, l);
      if(l > 40)
        lit++;
    }
  EXPECT(mx > 90, "nothing bright was drawn");
  // An absolute count, not a fraction of the canvas: a spectrogram fed a pure
  // tone lights one thin line, and a scrolling one only fills the columns it
  // has had time for.
  EXPECT(lit >= 32, "almost no pixel was drawn: only " + std::to_string(lit));
}

void expectAnimated(const Result& r)
{
  if(r.video.size() < 2)
  {
    EXPECT(false, "only one frame, cannot tell whether it animates");
    return;
  }
  EXPECT(
      meanDiff(r.video.front(), r.video.back()) > 0.5,
      "every frame is identical: it does not animate");
}

const std::vector<float>& chan(const Result& r, int out = 0, int c = 0)
{
  static const std::vector<float> empty;
  if(int(r.audio.size()) <= out || int(r.audio[std::size_t(out)].size()) <= c)
    return empty;
  return r.audio[std::size_t(out)][std::size_t(c)];
}

void expectAudio(const Result& r)
{
  EXPECT(!chan(r).empty(), "no audio came out");
  for(float x : chan(r))
    if(!std::isfinite(x))
    {
      fail("the output has non-finite samples");
      break;
    }
}

/// Input RMS of a 0.5-amplitude tone.
constexpr double toneRms = 0.5 / 1.41421356;

// Every preset, by file name.
std::map<std::string, Case> cases()
{
  std::map<std::string, Case> c;
  const auto vid = [](VIn in, int frames = 4) {
    Job j;
    j.vin = in;
    j.videoFrames = frames;
    return j;
  };
  const auto aud = [](std::vector<AIn> sigs, int blocks = 16) {
    Job j;
    j.ain = std::move(sigs);
    j.audioBlocks = blocks;
    return j;
  };

  // ---- video: the ones that must not change the picture -------------------
  c["Colour correction.scp"]
      = {vid(VIn::Pattern), [](const Result& r) {
           expectVideo(r);
           expectSize(r, VW, VH);
           if(r.video.empty())
             return;
           EXPECT(
               maxDiff(r.video.back(), inputImage(VIn::Pattern, 0)) <= 3,
               "the defaults are neutral, so the picture must come back unchanged");
         }};
  c["Hue and saturation.scp"]
      = {vid(VIn::Pattern), [](const Result& r) {
           expectVideo(r);
           if(r.video.empty())
             return;
           EXPECT(
               maxDiff(r.video.back(), inputImage(VIn::Pattern, 0)) <= 6,
               "h=0 s=1 is neutral, so the picture must come back unchanged");
         }};
  c["Rotate.scp"] = {vid(VIn::Pattern), [](const Result& r) {
                       expectVideo(r);
                       if(r.video.empty())
                         return;
                       EXPECT(
                           maxDiff(r.video.back(), inputImage(VIn::Pattern, 0)) <= 6,
                           "angle=0 must leave the picture where it was");
                     }};

  // ---- video: point operations -------------------------------------------
  c["Negate.scp"] = {vid(VIn::Pattern), [](const Result& r) {
                       expectVideo(r);
                       if(r.video.empty())
                         return;
                       const Img in = inputImage(VIn::Pattern, 0);
                       const Img& out = r.video.back();
                       double worst = 0;
                       for(int y = 0; y < VH; y++)
                         for(int x = 0; x < VW; x++)
                           for(int ch = 0; ch < 3; ch++)
                             worst = std::max(
                                 worst, std::abs(
                                            255. - in.at(x, y)[ch] - double(out.at(x, y)[ch])));
                       EXPECT(worst <= 3, "the output is not the inverse of the input");
                     }};
  c["Sepia.scp"] = {vid(VIn::Pattern), [](const Result& r) {
                      expectVideo(r);
                      if(r.video.empty())
                        return;
                      const Img in = inputImage(VIn::Pattern, 0);
                      const Img& out = r.video.back();
                      // The matrix in the preset, applied by hand.
                      double worst = 0;
                      for(int y = 0; y < VH; y += 4)
                        for(int x = 0; x < VW; x += 4)
                        {
                          const double R = in.at(x, y)[0], G = in.at(x, y)[1],
                                       B = in.at(x, y)[2];
                          const double e[3]
                              = {.393 * R + .769 * G + .189 * B, .349 * R + .686 * G + .168 * B,
                                 .272 * R + .534 * G + .131 * B};
                          for(int ch = 0; ch < 3; ch++)
                            worst = std::max(
                                worst,
                                std::abs(std::min(255., e[ch]) - double(out.at(x, y)[ch])));
                        }
                      EXPECT(worst <= 6, "the colour matrix was not applied");
                    }};
  c["Vintage curves.scp"]
      = {vid(VIn::Pattern), [](const Result& r) {
           expectVideo(r);
           if(r.video.empty())
             return;
           EXPECT(
               meanDiff(r.video.back(), inputImage(VIn::Pattern, 0)) > 3,
               "a curve preset that changes nothing is not applied");
         }};

  // ---- video: spatial ------------------------------------------------------
  const auto blurCheck = [](const Result& r) {
    expectVideo(r);
    if(r.video.empty())
      return;
    const Img in = inputImage(VIn::Pattern, 0);
    const Img& out = r.video.back();
    EXPECT(
        maxGradient(out, VH / 2) < maxGradient(in, VH / 2) * 0.8,
        "the black-to-white edge is as sharp as it was: nothing was blurred");
    int between = 0;
    for(int x = 24; x < 40; x++)
    {
      const double l = out.luma(x, VH / 2);
      if(l > 20 && l < 235)
        between++;
    }
    EXPECT(between >= 3, "no gradient across the edge: nothing was blurred");
  };
  c["Gaussian blur.scp"] = {vid(VIn::Pattern), blurCheck};
  c["Gaussian blur (Vulkan).scp"] = {vid(VIn::Pattern), blurCheck};
  c["Sharpen.scp"] = {vid(VIn::Pattern), [](const Result& r) {
                        expectVideo(r);
                        if(r.video.empty())
                          return;
                        const Img in = inputImage(VIn::Pattern, 0);
                        const Img& out = r.video.back();
                        // An unsharp mask overshoots: darker just before the
                        // edge, brighter just after.
                        EXPECT(
                            out.luma(30, VH / 2) <= in.luma(30, VH / 2) + 1
                                && out.luma(33, VH / 2) >= in.luma(33, VH / 2) - 1,
                            "no overshoot around the edge: nothing was sharpened");
                        EXPECT(
                            meanDiff(out, in) > 0.2, "the picture came back untouched");
                      }};
  c["Edge detect.scp"]
      = {vid(VIn::Pattern), [](const Result& r) {
           expectVideo(r);
           if(r.video.empty())
             return;
           const Img& out = r.video.back();
           // An edge map is sparse: mostly black, with bright pixels only
           // where the input changes colour (x = 16, 32 and 48).
           double mx = 0;
           int mxx = 0;
           for(int y = 0; y < out.h; y++)
             for(int x = 0; x < out.w; x++)
               if(out.luma(x, y) > mx)
               {
                 mx = out.luma(x, y);
                 mxx = x;
               }
           EXPECT(mx > 200, "nothing was detected as an edge");
           const bool onABorder = std::abs(mxx - 16) <= 2 || std::abs(mxx - 32) <= 2
                                  || std::abs(mxx - 48) <= 2;
           EXPECT(
               onABorder,
               "the brightest pixel is at x=" + std::to_string(mxx)
                   + ", nowhere near a border of the test pattern");
           EXPECT(out.meanLuma() < 40, "the output is not a sparse edge map");
         }};
  c["Pixelate.scp"]
      = {vid(VIn::Pattern), [](const Result& r) {
           expectVideo(r);
           if(r.video.empty())
             return;
           const Img& out = r.video.back();
           // 16x16 blocks: inside the ramp band, every block must be flat.
           for(int by = 0; by + 16 <= VH; by += 16)
           {
             const double ref = out.luma(48, by);
             for(int y = by; y < by + 16; y++)
               for(int x = 48; x < 64; x++)
                 if(std::abs(out.luma(x, y) - ref) > 6)
                 {
                   fail("a 16x16 block is not flat: the ramp was not pixelated");
                   return;
                 }
           }
         }};
  c["Vignette.scp"] = {vid(VIn::Uniform), [](const Result& r) {
                         expectVideo(r);
                         if(r.video.empty())
                           return;
                         const Img& out = r.video.back();
                         const double centre = out.luma(VW / 2, VH / 2);
                         const double corner = out.luma(1, 1);
                         EXPECT(
                             corner < centre * 0.85,
                             "the corners are as bright as the centre: no vignette");
                       }};
  c["Film grain.scp"]
      = {vid(VIn::Uniform), [](const Result& r) {
           expectVideo(r);
           if(r.video.empty())
             return;
           const Img& out = r.video.back();
           EXPECT(out.varianceLuma() > 20, "a flat field came back flat: no grain added");
           EXPECT(
               std::abs(out.meanLuma() - 128) < 20,
               "the grain moved the average brightness far off");
         }};
  // hqdn3d is mostly a TEMPORAL denoiser: it needs a few frames of moving
  // noise over a still image before the average settles.
  c["Denoise.scp"] = {vid(VIn::Noisy, 16), [](const Result& r) {
                        expectVideo(r);
                        if(r.video.empty())
                          return;
                        const double in = inputImage(VIn::Noisy, 15).varianceLuma();
                        const double out = r.video.back().varianceLuma();
                        EXPECT(
                            out < in * 0.9, "the noise is still there: variance went from "
                                                + std::to_string(int(in)) + " to "
                                                + std::to_string(int(out)));
                      }};
  c["Denoise (Vulkan).scp"] = {vid(VIn::Noisy), [](const Result& r) {
                                 expectVideo(r);
                                 if(r.video.empty())
                                   return;
                                 const Img in = inputImage(VIn::Noisy, 0);
                                 EXPECT(
                                     r.video.back().varianceLuma() < in.varianceLuma() * 0.9,
                                     "the noise is still there");
                               }};
  c["Deband.scp"] = {vid(VIn::Pattern), [](const Result& r) {
                       expectVideo(r);
                       if(r.video.empty())
                         return;
                       EXPECT(
                           meanDiff(r.video.back(), inputImage(VIn::Pattern, 0)) < 12,
                           "debanding must not repaint a clean picture");
                     }};
  c["Chroma shift.scp"]
      = {vid(VIn::Pattern), [](const Result& r) {
           expectVideo(r);
           if(r.video.empty())
             return;
           const Img in = inputImage(VIn::Pattern, 0);
           const Img& out = r.video.back();
           EXPECT(meanDiff(out, in) > 1, "the chroma was not moved");
           // Luma is untouched by a chroma shift: the white band stays white.
           EXPECT(out.luma(40, VH / 2) > 200, "the luma was damaged");
         }};

  // ---- video: keying -------------------------------------------------------
  const auto keyCheck = [](const Result& r) {
    expectVideo(r);
    if(r.video.empty())
      return;
    const Img& out = r.video.back();
    EXPECT(out.at(4, VH / 2)[3] < 40, "the green area was not keyed out");
    // blend=0.1 softens the edge of the key, so what is kept sits below 255.
    EXPECT(out.at(40, VH / 2)[3] > 150, "the white area was keyed out as well");
  };
  c["Chroma key.scp"] = {vid(VIn::Pattern), keyCheck};
  c["Colour key.scp"] = {vid(VIn::Pattern), keyCheck};
  c["Chroma key (CUDA).scp"] = {vid(VIn::Pattern), keyCheck};

  // ---- video: temporal -----------------------------------------------------
  c["Trails.scp"]
      = {vid(VIn::Alternating, 12), [](const Result& r) {
           expectVideo(r);
           if(r.video.empty())
             return;
           // Eight frames averaged, half black and half white: mid grey.
           const double m = r.video.back().meanLuma();
           EXPECT(
               m > 96 && m < 160,
               "averaging eight alternating black and white frames must give grey, got "
                   + std::to_string(int(m)));
         }};
  c["Frame difference.scp"]
      = {vid(VIn::Alternating, 6), [](const Result& r) {
           expectVideo(r, 2);
           if(r.video.size() < 2)
             return;
           // Consecutive frames are opposite, so the difference is white.
           EXPECT(
               r.video.back().meanLuma() > 200,
               "the difference between a black and a white frame must be white");
         }};
  c["Deflicker.scp"] = {vid(VIn::Uniform, 8), [](const Result& r) {
                          expectVideo(r);
                          if(r.video.empty())
                            return;
                          EXPECT(
                              std::abs(r.video.back().meanLuma() - 128) < 6,
                              "a steady input must come out steady");
                        }};
  c["Stabilise.scp"] = {vid(VIn::Pattern, 6), [](const Result& r) {
                          expectVideo(r);
                          expectSize(r, VW, VH);
                          if(r.video.empty())
                            return;
                          EXPECT(
                              meanDiff(r.video.back(), inputImage(VIn::Pattern, 0)) < 30,
                              "a static shot must not be moved around");
                        }};
  c["Motion interpolation.scp"]
      = {vid(VIn::Alternating, 8), [](const Result& r) {
           expectVideo(r);
           expectSize(r, VW, VH);
           if(r.video.size() < 2)
             return;
           double brightest = 0;
           for(const auto& f : r.video)
             brightest = std::max(brightest, f.meanLuma());
           EXPECT(brightest > 100, "every frame came out black");
         }};
  c["Deinterlace (Vulkan).scp"]
      = {vid(VIn::Pattern, 4), [](const Result& r) {
           expectVideo(r);
           expectSize(r, VW, VH);
           if(r.video.empty())
             return;
           EXPECT(
               meanDiff(r.video.back(), inputImage(VIn::Pattern, 0)) < 40,
               "a progressive picture must survive the deinterlacer");
         }};
  c["Crossfade (Vulkan).scp"]
      = {[] {
           Job j;
           j.vin = VIn::Alternating; // both inputs get the same material
           j.videoFrames = 8;
           return j;
         }(),
         [](const Result& r) {
           expectVideo(r);
           expectSize(r, VW, VH);
         }};
  c["360 reprojection (Vulkan).scp"] = {vid(VIn::Pattern), [](const Result& r) {
                                          expectVideo(r);
                                          expectDetail(r);
                                        }};

  // ---- video: generators ---------------------------------------------------
  c["Test pattern.scp"] = {vid(VIn::None, 3), [](const Result& r) {
                             expectVideo(r);
                             expectSize(r, 1280, 720);
                             expectDetail(r);
                             expectAnimated(r);
                           }};
  c["SMPTE bars.scp"]
      = {vid(VIn::None, 2), [](const Result& r) {
           expectVideo(r);
           expectSize(r, 1280, 720);
           if(r.video.empty())
             return;
           // Colour bars: several flat vertical bands across the top.
           const Img& img = r.video.front();
           int bands = 1;
           for(int x = 1; x < img.w; x++)
             if(std::abs(img.luma(x, 20) - img.luma(x - 1, 20)) > 8)
               bands++;
           EXPECT(bands >= 6, "fewer than six colour bars, got " + std::to_string(bands));
         }};
  c["Gradients.scp"] = {vid(VIn::None, 4), [](const Result& r) {
                          expectVideo(r);
                          expectSize(r, 1280, 720);
                          expectDetail(r);
                          expectAnimated(r);
                        }};
  c["Mandelbrot.scp"] = {vid(VIn::None, 3), [](const Result& r) {
                           expectVideo(r);
                           expectSize(r, 1280, 720);
                           expectDetail(r);
                           expectAnimated(r);
                         }};
  c["Game of life.scp"] = {vid(VIn::None, 4), [](const Result& r) {
                             expectVideo(r);
                             expectSize(r, 320, 240);
                             expectDetail(r);
                           }};
  c["Cellular automaton.scp"] = {vid(VIn::None, 4), [](const Result& r) {
                                   expectVideo(r);
                                   expectSize(r, 640, 480);
                                   expectDetail(r);
                                 }};

  // ---- video: analysis, results on the Metadata outlet ---------------------
  c["Black detection.scp"]
      = {vid(VIn::Black, 40), [](const Result& r) {
           expectVideo(r);
           EXPECT(
               hasMeta(r, "black_start"),
               "40 black frames and no lavfi.black_start; keys: " + metaKeys(r));
         }};
  c["Freeze detection.scp"]
      = {vid(VIn::Static, 150), [](const Result& r) {
           expectVideo(r);
           EXPECT(
               hasMeta(r, "freeze"),
               "the same frame for 2.5 s and no freeze reported; keys: " + metaKeys(r));
         }};
  c["Crop detection.scp"]
      = {vid(VIn::Bordered, 8), [](const Result& r) {
           expectVideo(r);
           const auto* w = metaValue(r, "cropdetect.w");
           const auto* h = metaValue(r, "cropdetect.h");
           EXPECT(w && h, "no crop rectangle reported; keys: " + metaKeys(r));
           if(w && h)
           {
             const int cw = std::atoi(w->c_str()), ch = std::atoi(h->c_str());
             EXPECT(
                 cw <= VW - 16 && ch <= VH - 8,
                 "the borders were not detected: " + *w + "x" + *h);
           }
         }};
  c["Signal statistics.scp"]
      = {vid(VIn::Uniform, 3), [](const Result& r) {
           expectVideo(r);
           const auto* y = metaValue(r, "signalstats.YAVG");
           EXPECT(y != nullptr, "no YAVG reported; keys: " + metaKeys(r));
           if(y)
           {
             // A flat 128 grey is Y=126 in limited-range BT.601.
             const double v = std::atof(y->c_str());
             EXPECT(
                 v > 100 && v < 150,
                 "YAVG is " + *y + " for a flat mid-grey input");
           }
         }};
  c["Entropy.scp"] = {vid(VIn::Pattern, 3), [](const Result& r) {
                        expectVideo(r);
                        const auto* e = metaValue(r, "entropy");
                        EXPECT(e != nullptr, "no entropy reported; keys: " + metaKeys(r));
                        if(e)
                        {
                          const double v = std::atof(e->c_str());
                          EXPECT(v > 0 && v <= 8.01, "entropy out of range: " + *e);
                        }
                      }};
  c["Scene detection.scp"]
      = {vid(VIn::Alternating, 6), [](const Result& r) {
           expectVideo(r);
           const double best = metaMax(r, "scd.score");
           EXPECT(best > -1e29, "no scene score reported; keys: " + metaKeys(r));
           EXPECT(
               best > 10,
               "black to white is a scene change, but the best score is "
                   + std::to_string(best));
         }};

  // ---- audio: filters ------------------------------------------------------
  c["High-pass.scp"] = {aud({AIn::Sine100}), [](const Result& r) {
                          expectAudio(r);
                          EXPECT(
                              rms(chan(r), 4096) < toneRms * 0.6,
                              "a 100 Hz tone survived a 200 Hz high-pass");
                        }};
  c["Low-pass.scp"] = {aud({AIn::Sine8k}), [](const Result& r) {
                         expectAudio(r);
                         EXPECT(
                             rms(chan(r), 4096) < toneRms * 0.6,
                             "an 8 kHz tone survived a 4 kHz low-pass");
                       }};
  c["Parametric band.scp"] = {aud({AIn::Sine1k}), [](const Result& r) {
                                expectAudio(r);
                                EXPECT(
                                    std::abs(rms(chan(r), 4096) - toneRms) < toneRms * 0.15,
                                    "a 0 dB band must leave the level alone");
                              }};
  c["Equalizer.scp"] = {aud({AIn::Sine200}), [](const Result& r) {
                          expectAudio(r);
                          const double out = rms(chan(r), 4096);
                          EXPECT(
                              out < toneRms * 0.8 && out > toneRms * 0.2,
                              "a -6 dB band at 200 Hz did not attenuate a 200 Hz tone");
                        }};
  c["Compressor.scp"] = {aud({AIn::Loud}), [](const Result& r) {
                           expectAudio(r);
                           EXPECT(
                               rms(chan(r), 4096) < 0.707 * 0.9,
                               "a full-scale tone came through a 4:1 compressor untouched");
                         }};
  c["Limiter.scp"] = {aud({AIn::Loud}), [](const Result& r) {
                        expectAudio(r);
                        EXPECT(peak(chan(r)) <= 0.92, "the limiter let the signal past 0.9");
                      }};
  c["Noise gate.scp"] = {aud({AIn::Quiet}), [](const Result& r) {
                           expectAudio(r);
                           EXPECT(
                               rms(chan(r), 4096) < 0.0005,
                               "a -60 dBFS signal went straight through the gate");
                         }};
  c["De-esser.scp"] = {aud({AIn::Sine440}), [](const Result& r) {
                         expectAudio(r);
                         EXPECT(
                             rms(chan(r), 2048) > toneRms * 0.3,
                             "the de-esser removed a plain 440 Hz tone");
                       }};
  c["Spectral denoise.scp"] = {aud({AIn::Noise}, 48), [](const Result& r) {
                                 expectAudio(r);
                                 const double in = 0.2886; // rms of the test noise
                                 EXPECT(
                                     rms(chan(r), 12000) < in * 0.95,
                                     "the noise came through the denoiser at full level");
                               }};
  c["Loudness normalise.scp"]
      = {aud({AIn::Quiet}, 400), [](const Result& r) {
           expectAudio(r);
           EXPECT(
               rms(chan(r), 4096) > 0.001,
               "a very quiet input must be brought up towards the target");
         }};
  c["Time stretch.scp"] = {aud({AIn::Sine440}), [](const Result& r) {
                             expectAudio(r);
                             EXPECT(
                                 std::abs(rms(chan(r), 2048) - toneRms) < toneRms * 0.15,
                                 "atempo=1 must not change the signal");
                             EXPECT(
                                 toneAt(chan(r), 440) > toneAt(chan(r), 880),
                                 "the tone is no longer at 440 Hz");
                           }};
  c["Frequency shift.scp"]
      = {aud({AIn::Sine440}), [](const Result& r) {
           expectAudio(r);
           EXPECT(
               toneAt(chan(r), 540) > toneAt(chan(r), 440) * 2,
               "a 100 Hz shift must move a 440 Hz tone to 540 Hz");
         }};
  c["Bit crusher.scp"] = {aud({AIn::Sine440}), [](const Result& r) {
                            expectAudio(r);
                            EXPECT(
                                rms(chan(r), 2048) > toneRms * 0.4,
                                "the crusher destroyed the signal");
                            EXPECT(
                                toneAt(chan(r), 1320) > 1e-4,
                                "quantisation must add harmonics, and there are none");
                          }};
  c["Tremolo.scp"]
      = {aud({AIn::Sine440}, 100), [](const Result& r) {
           expectAudio(r);
           const auto& v = chan(r);
           if(v.size() < 40000)
           {
             EXPECT(false, "not enough audio to see the modulation");
             return;
           }
           // 5 Hz at 48 kHz: peak and trough are 4800 samples apart.
           double lo = 1e9, hi = 0;
           for(std::size_t i = 10000; i + 2400 < v.size(); i += 2400)
           {
             const double e = rms(v, i, i + 2400);
             lo = std::min(lo, e);
             hi = std::max(hi, e);
           }
           EXPECT(hi > lo * 1.5, "the level does not move: no tremolo");
         }};
  // FFmpeg 6.1's vibrato reads its delay line before filling it and can emit
  // NaNs for the first few milliseconds; the wrapper silences those, and
  // expectAudio is what checks that nothing non-finite gets through.
  c["Vibrato.scp"] = {aud({AIn::Sine440}, 40), [](const Result& r) {
                        expectAudio(r);
                        EXPECT(
                            rms(chan(r), 8192) > toneRms * 0.7,
                            "the level should barely change");
                        // 5 Hz of frequency modulation moves energy off the
                        // carrier and into sidebands around it.
                        const double carrier = toneAt(chan(r), 440);
                        const double sidebands
                            = toneAt(chan(r), 430) + toneAt(chan(r), 450);
                        EXPECT(
                            sidebands > carrier * 0.5,
                            "no sidebands: carrier " + std::to_string(carrier)
                                + ", sidebands " + std::to_string(sidebands));
                      }};
  const auto modulated = [](const Result& r) {
    expectAudio(r);
    EXPECT(rms(chan(r), 4096) > 0.05, "nothing came out");
    EXPECT(
        std::abs(toneAt(chan(r), 440) - 0.5) > 0.01,
        "the tone came through untouched: the effect did nothing");
  };
  c["Chorus.scp"] = {aud({AIn::Sine440}, 32), modulated};
  c["Flanger.scp"] = {aud({AIn::Sine440}, 32), modulated};
  c["Phaser.scp"] = {aud({AIn::Sine440}, 32), modulated};
  c["Reverb.scp"] = {aud({AIn::Burst}, 32), [](const Result& r) {
                       expectAudio(r);
                       const auto& v = chan(r);
                       EXPECT(
                           v.size() > 8192 && rms(v, 4096) > 1e-4,
                           "nothing is left after the burst: no echo tail");
                     }};
  c["Delay.scp"] = {aud({AIn::Burst}, 64), [](const Result& r) {
                      expectAudio(r);
                      const auto& v = chan(r);
                      // 500 ms at 48 kHz: the repeat lands around sample 24000.
                      EXPECT(
                          v.size() > 26000 && rms(v, 23000, 26000) > 1e-3,
                          "no repeat half a second after the burst");
                    }};
  c["Stereo widener.scp"]
      = {aud({AIn::Wide}, 32), [](const Result& r) {
           expectAudio(r);
           const auto& l = chan(r, 0, 0);
           const auto& rr = chan(r, 0, 1);
           if(l.size() < 4096 || rr.size() < 4096)
           {
             EXPECT(false, "expected two channels out");
             return;
           }
           std::vector<float> side(l.size());
           for(std::size_t i = 0; i < l.size(); i++)
             side[i] = l[i] - rr[i];
           // In: side is 0.25 amplitude. m=2.5 must widen it.
           EXPECT(
               rms(side, 2048) > 0.25 / 1.414 * 1.5,
               "the side signal was not widened");
         }};
  c["Mono downmix.scp"] = {aud({AIn::Wide}, 16), [](const Result& r) {
                             expectAudio(r);
                             EXPECT(r.channels == 1, "the output is not mono");
                             EXPECT(
                                 rms(chan(r), 2048) > 0.2, "the downmix is silent");
                           }};
  c["Crossover.scp"]
      = {aud({AIn::Sine100}, 24), [](const Result& r) {
           expectAudio(r);
           EXPECT(r.audio.size() == 2, "a crossover has two outputs");
           if(r.audio.size() < 2 || r.audio[1].empty())
             return;
           const double low = rms(chan(r, 0), 4096);
           const double high = rms(chan(r, 1), 4096);
           EXPECT(
               low > high * 4,
               "a 100 Hz tone must come out of the low band, not the high one");
         }};
  c["Sidechain compressor.scp"]
      = {aud({AIn::Sine440, AIn::Loud}, 32), [](const Result& r) {
           expectAudio(r);
           EXPECT(
               rms(chan(r), 8192) < toneRms * 0.8,
               "a full-scale key signal must duck the main input");
         }};
  c["Mix.scp"] = {aud({AIn::Sine440, AIn::Sine440}, 16), [](const Result& r) {
                    expectAudio(r);
                    EXPECT(
                        rms(chan(r), 2048) > toneRms * 1.6,
                        "two identical inputs must add up (normalize=0)");
                  }};

  // ---- audio: generators ---------------------------------------------------
  c["Sine.scp"] = {aud({}, 16), [](const Result& r) {
                     expectAudio(r);
                     // The lavfi generator emits at -18 dBFS (amplitude 1/8).
                     EXPECT(
                         toneAt(chan(r), 440) > 0.05,
                         "the generator is not producing a 440 Hz tone");
                     EXPECT(
                         toneAt(chan(r), 440) > toneAt(chan(r), 660) * 10,
                         "it is not a clean tone");
                   }};
  c["Pink noise.scp"] = {aud({}, 32), [](const Result& r) {
                           expectAudio(r);
                           const auto& v = chan(r);
                           EXPECT(rms(v, 4096) > 0.05, "the generator is silent");
                           EXPECT(
                               bandEnergy(v, 60, 300) > bandEnergy(v, 3000, 15000) * 2,
                               "pink noise must fall off with frequency");
                         }};

  // ---- audio: analysis -----------------------------------------------------
  c["Audio statistics.scp"]
      = {aud({AIn::Sine440}, 24), [](const Result& r) {
           expectAudio(r);
           const auto* v = metaValue(r, "RMS_level");
           EXPECT(v != nullptr, "no RMS level reported; keys: " + metaKeys(r));
           if(v)
           {
             const double db = std::atof(v->c_str());
             const double expected = 20 * std::log10(toneRms);
             EXPECT(
                 std::abs(db - expected) < 3,
                 "reported " + *v + " dB, the input is " + std::to_string(expected));
           }
         }};
  c["Loudness meter.scp"]
      = {aud({AIn::Sine440}, 200), [](const Result& r) {
           expectAudio(r);
           const auto* v = metaValue(r, "r128.M");
           EXPECT(v != nullptr, "no momentary loudness reported; keys: " + metaKeys(r));
           if(v)
           {
             // -9 dBFS sine, K-weighted around 440 Hz: about -12 LUFS.
             const double m = std::atof(v->c_str());
             EXPECT(m > -20 && m < -5, "momentary loudness is " + *v + " LUFS");
           }
         }};
  c["Phase meter.scp"]
      = {aud({AIn::Sine440}, 24), [](const Result& r) {
           expectAudio(r);
           const auto* v = metaValue(r, "phase");
           EXPECT(v != nullptr, "no phase reported; keys: " + metaKeys(r));
           if(v)
             EXPECT(
                 std::atof(v->c_str()) > 0.9,
                 "both channels carry the same signal, so the phase must be +1, got " + *v);
         }};
  c["Silence detection.scp"]
      = {aud({AIn::Silence}, 100), [](const Result& r) {
           expectAudio(r);
           EXPECT(
               hasMeta(r, "silence_start"),
               "a second of silence and nothing reported; keys: " + metaKeys(r));
         }};

  // ---- audio in, video out -------------------------------------------------
  const auto scope = [](int w, int h) {
    return [w, h](const Result& r) {
      expectVideo(r);
      expectSize(r, w, h);
      expectLit(r);
    };
  };
  c["Waveform scope.scp"] = {aud({AIn::Sine440}, 24), scope(1024, 256)};
  c["Vector scope.scp"] = {aud({AIn::Wide}, 24), scope(512, 512)};
  // showvolume stacks one meter per channel, so two channels are taller than h.
  c["Volume meter.scp"] = {aud({AIn::Sine440}, 24), [](const Result& r) {
                             expectVideo(r);
                             if(!r.video.empty())
                               EXPECT(
                                   r.video[0].w == 720 && r.video[0].h >= 68,
                                   "the meter is not 720 wide with a row per channel");
                             expectLit(r);
                           }};
  c["Constant-Q spectrum.scp"] = {aud({AIn::Sine440}, 64), scope(960, 480)};
  c["Frequency bars.scp"] = {aud({AIn::Sine440}, 24), scope(1024, 256)};
  c["Spectrum.scp"] = {aud({AIn::Sine440}, 256), scope(1024, 512)};

  return c;
}
}

int main(int argc, char** argv)
{
  const std::string dir = argc > 1 ? argv[1] :
#ifdef LAVFI_PRESET_DIR
                                   LAVFI_PRESET_DIR
#else
                                   ""
#endif
      ;
  if(dir.empty() || !std::filesystem::exists(dir))
  {
    std::fprintf(stderr, "preset directory not found: '%s'\n", dir.c_str());
    return 2;
  }

  auto specs = cases();

  std::vector<std::filesystem::path> files;
  for(const auto& de : std::filesystem::directory_iterator(dir))
    if(de.path().extension() == ".scp")
      files.push_back(de.path());
  std::sort(files.begin(), files.end());

  int checked = 0, skipped = 0;
  for(const auto& path : files)
  {
    const std::string file = path.filename().string();
    current = file;

    const std::string doc = readFile(path);
    const std::string text = jsonString(doc, "Effect");
    if(text.empty())
    {
      fail("no graph in the preset file");
      continue;
    }

    auto it = specs.find(file);
    if(it == specs.end())
    {
      fail("no behaviour check is written for this preset");
      continue;
    }

    Job job = it->second.job;
    job.text = text;
    const Result r = run(job);
    if(r.skipped)
    {
      std::printf("  skip %-30s %s\n", file.c_str(), r.error.c_str());
      skipped++;
      continue;
    }
    if(!r.configured)
    {
      fail("did not configure: " + r.error);
      continue;
    }

    const int before = failures;
    it->second.check(r);
    if(failures == before)
    {
      std::printf("  ok   %s\n", file.c_str());
      checked++;
    }
  }

  current = "(suite)";
  if(checked + skipped != int(files.size()))
    fail(
        "checked " + std::to_string(checked) + " and skipped " + std::to_string(skipped)
        + " of " + std::to_string(files.size()) + " presets");

  std::printf(
      "lavfi preset behaviour: %s (%d failure%s); %d checked, %d skipped\n",
      failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s", checked, skipped);
  return failures ? 1 : 0;
}
