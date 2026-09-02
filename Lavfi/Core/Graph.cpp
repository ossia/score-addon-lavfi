#include "Graph.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavcodec/defs.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
}

// The segment API (avfilter_graph_segment_*) appeared in lavfi 9.7 (FFmpeg 6.1).
// It is what lets hw_device_ctx be set on filters before their init(),
// which hwupload/hwmap need; avfilter_graph_parse2 inits during parsing.
#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(9, 7, 100)
#error "score-addon-lavfi needs FFmpeg >= 6.1 (libavfilter >= 9.7)"
#endif

// buffersink's format options were renamed in lavfi 10.3 (FFmpeg 7.1) from the
// binary-array pix_fmts/sample_fmts/sample_rates to string-array
// pixel_formats/sample_formats/samplerates; the old ones are gone in 8.0.
#define LAVFI_HAS_NEW_SINK_OPTS (LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(10, 3, 100))

namespace Lavfi
{

// ---------------------------------------------------------------------------
//  av_log capture, keyed on the AVFilterGraph a message belongs to.
// ---------------------------------------------------------------------------
struct LogRegistry
{
  std::mutex mutex;
  std::unordered_map<const AVFilterGraph*, Graph*> graphs;
  void (*previous)(void*, int, const char*, va_list){};
  bool installed{};

  static LogRegistry& instance()
  {
    static LogRegistry r;
    return r;
  }

  static const AVFilterGraph* graphOf(void* avcl) noexcept
  {
    if(!avcl)
      return nullptr;
    const AVClass* cls = *static_cast<const AVClass**>(avcl);
    if(!cls)
      return nullptr;
    if(cls == avfilter_get_class())
      return static_cast<const AVFilterContext*>(avcl)->graph;
    if(cls->class_name && std::strcmp(cls->class_name, "AVFilterGraph") == 0)
      return static_cast<const AVFilterGraph*>(avcl);
    // Filters log through their own AVClass whose parent is the AVFilterContext.
    if(cls->parent_log_context_offset)
    {
      auto parent = *reinterpret_cast<void**>(
          static_cast<char*>(avcl) + cls->parent_log_context_offset);
      return graphOf(parent);
    }
    return nullptr;
  }

  static void callback(void* avcl, int level, const char* fmt, va_list vl)
  {
    auto& self = instance();
    if(level <= AV_LOG_WARNING)
    {
      if(auto g = graphOf(avcl))
      {
        std::lock_guard lock{self.mutex};
        if(auto it = self.graphs.find(g); it != self.graphs.end())
        {
          char buf[1024];
          Graph& graph = *it->second;
          av_log_format_line2(avcl, level, fmt, vl, buf, sizeof(buf), &graph.m_logPrefix);
          graph.m_logLine += buf;
          if(!graph.m_logLine.empty() && graph.m_logLine.back() == '\n')
          {
            if(graph.m_log.size() < 16384)
              graph.m_log += graph.m_logLine;
            graph.m_logLine.clear();
          }
          return;
        }
      }
    }
    if(self.previous)
      self.previous(avcl, level, fmt, vl);
    else
      av_log_default_callback(avcl, level, fmt, vl);
  }

  void add(const AVFilterGraph* g, Graph* self)
  {
    std::lock_guard lock{mutex};
    if(!installed)
    {
      // libavutil has no getter for the current callback, so a previously
      // installed one cannot be chained; score does not install any, and
      // everything that is not ours goes to av_log_default_callback.
      av_log_set_callback(&LogRegistry::callback);
      installed = true;
    }
    graphs[g] = self;
  }
  void remove(const AVFilterGraph* g)
  {
    std::lock_guard lock{mutex};
    graphs.erase(g);
  }
};

// ---------------------------------------------------------------------------

bool Description::hasVideo() const noexcept
{
  for(auto& p : inputs)
    if(p.type == AVMEDIA_TYPE_VIDEO)
      return true;
  for(auto& p : outputs)
    if(p.type == AVMEDIA_TYPE_VIDEO)
      return true;
  return false;
}

bool Description::hasAudio() const noexcept
{
  for(auto& p : inputs)
    if(p.type == AVMEDIA_TYPE_AUDIO)
      return true;
  for(auto& p : outputs)
    if(p.type == AVMEDIA_TYPE_AUDIO)
      return true;
  return false;
}

static std::string errString(int err)
{
  char buf[AV_ERROR_MAX_STRING_SIZE]{};
  av_strerror(err, buf, sizeof(buf));
  return buf;
}

const char* optionTypeName(AVOptionType t) noexcept
{
  switch(t)
  {
    case AV_OPT_TYPE_FLAGS: return "flags";
    case AV_OPT_TYPE_INT: return "int";
    case AV_OPT_TYPE_INT64: return "int64";
    case AV_OPT_TYPE_DOUBLE: return "double";
    case AV_OPT_TYPE_FLOAT: return "float";
    case AV_OPT_TYPE_STRING: return "string";
    case AV_OPT_TYPE_RATIONAL: return "rational";
    case AV_OPT_TYPE_BINARY: return "binary";
    case AV_OPT_TYPE_DICT: return "dict";
    case AV_OPT_TYPE_UINT64: return "uint64";
    case AV_OPT_TYPE_CONST: return "const";
    case AV_OPT_TYPE_IMAGE_SIZE: return "image_size";
    case AV_OPT_TYPE_PIXEL_FMT: return "pixel_fmt";
    case AV_OPT_TYPE_SAMPLE_FMT: return "sample_fmt";
    case AV_OPT_TYPE_VIDEO_RATE: return "video_rate";
    case AV_OPT_TYPE_DURATION: return "duration";
    case AV_OPT_TYPE_COLOR: return "color";
    case AV_OPT_TYPE_BOOL: return "bool";
    case AV_OPT_TYPE_CHLAYOUT: return "channel_layout";
    default: return "other";
  }
}

bool hasFilter(const char* name) noexcept
{
  return avfilter_get_by_name(name) != nullptr;
}

// ---------------------------------------------------------------------------

Graph::Graph() = default;

Graph::~Graph()
{
  destroy();
}

void Graph::destroy()
{
  for(auto& in : m_inputs)
  {
    av_frame_free(&in.frame);
    av_buffer_pool_uninit(&in.pool);
  }
  for(auto& out : m_outputs)
    av_frame_free(&out.frame);
  m_inputs.clear();
  m_outputs.clear();
  if(m_graph)
  {
    LogRegistry::instance().remove(m_graph);
    avfilter_graph_free(&m_graph);
  }
  m_eof = false;
}

bool Graph::describe(const std::string& text, Description& out, std::string& error)
{
  Graph g;
  if(!g.build(text, nullptr, nullptr, nullptr, false, error))
    return false;
  out = g.m_desc;
  return true;
}

bool Graph::init(
    const std::string& text, const std::vector<InputConfig>& inputs,
    const SinkConfig& sinks, AVBufferRef* hw_device, std::string& error)
{
  destroy();
  return build(text, &inputs, &sinks, hw_device, true, error);
}

static std::string channelLayoutString(int channels)
{
  AVChannelLayout layout{};
  av_channel_layout_default(&layout, channels);
  char buf[128]{};
  if(av_channel_layout_describe(&layout, buf, sizeof(buf)) < 0 || !buf[0])
    std::snprintf(buf, sizeof(buf), "%dC", channels);
  av_channel_layout_uninit(&layout);
  return buf;
}

bool Graph::build(
    const std::string& text, const std::vector<InputConfig>* inputs,
    const SinkConfig* sinks, AVBufferRef* hw_device, bool configure,
    std::string& error)
{
  error.clear();
  m_graph = avfilter_graph_alloc();
  if(!m_graph)
  {
    error = "avfilter_graph_alloc failed";
    return false;
  }
  LogRegistry::instance().add(m_graph, this);
  if(sinks)
  {
    m_graph->nb_threads = sinks->threads;
    m_graph->thread_type = sinks->threads == 1 ? 0 : AVFILTER_THREAD_SLICE;
  }

  auto fail = [&](const char* what, int err) {
    error = what;
    if(err)
      error += ": " + errString(err);
    if(auto log = takeLog(); !log.empty())
      error += "\n" + log;
    LogRegistry::instance().remove(m_graph);
    avfilter_graph_free(&m_graph);
    m_inputs.clear();
    m_outputs.clear();
    return false;
  };

  // --- parse -> create -> options -> (hw device) -> init -> link -----------
  AVFilterGraphSegment* seg = nullptr;
  int ret = avfilter_graph_segment_parse(m_graph, text.c_str(), 0, &seg);
  if(ret < 0)
    return fail("parse error", ret);
  struct SegGuard
  {
    AVFilterGraphSegment*& s;
    ~SegGuard() { avfilter_graph_segment_free(&s); }
  } segGuard{seg};

  if((ret = avfilter_graph_segment_create_filters(seg, 0)) < 0)
    return fail("could not create filters", ret);
  if((ret = avfilter_graph_segment_apply_opts(seg, 0)) < 0)
    return fail("bad filter options", ret);
  if(hw_device)
  {
    for(unsigned i = 0; i < m_graph->nb_filters; i++)
    {
      AVFilterContext* f = m_graph->filters[i];
      if(!f->hw_device_ctx)
        f->hw_device_ctx = av_buffer_ref(hw_device);
    }
  }
  if((ret = avfilter_graph_segment_init(seg, 0)) < 0)
    return fail("filter initialisation failed", ret);

  AVFilterInOut* open_inputs = nullptr;
  AVFilterInOut* open_outputs = nullptr;
  if((ret = avfilter_graph_segment_link(seg, 0, &open_inputs, &open_outputs)) < 0)
    return fail("could not link filters", ret);
  struct InOutGuard
  {
    AVFilterInOut*& a;
    AVFilterInOut*& b;
    ~InOutGuard()
    {
      avfilter_inout_free(&a);
      avfilter_inout_free(&b);
    }
  } inoutGuard{open_inputs, open_outputs};

  // --- description ----------------------------------------------------------
  m_desc = {};
  int idx = 0;
  for(auto* io = open_inputs; io; io = io->next, idx++)
  {
    PadInfo p;
    p.type = avfilter_pad_get_type(io->filter_ctx->input_pads, io->pad_idx);
    p.filter = io->filter_ctx->name ? io->filter_ctx->name : "";
    p.pad = io->pad_idx;
    p.name = io->name ? io->name
                      : std::string(p.type == AVMEDIA_TYPE_AUDIO ? "audio in " : "video in ")
                            + std::to_string(idx);
    m_desc.inputs.push_back(std::move(p));
  }
  idx = 0;
  for(auto* io = open_outputs; io; io = io->next, idx++)
  {
    PadInfo p;
    p.type = avfilter_pad_get_type(io->filter_ctx->output_pads, io->pad_idx);
    p.filter = io->filter_ctx->name ? io->filter_ctx->name : "";
    p.pad = io->pad_idx;
    p.name = io->name ? io->name
                      : std::string(p.type == AVMEDIA_TYPE_AUDIO ? "audio out " : "video out ")
                            + std::to_string(idx);
    m_desc.outputs.push_back(std::move(p));
  }
  collectDescription();

  if(!configure)
    return true;

  // --- sources ---------------------------------------------------------------
  if(!inputs || inputs->size() != m_desc.inputs.size())
    return fail("input configuration does not match the graph's open inputs", 0);

  idx = 0;
  for(auto* io = open_inputs; io; io = io->next, idx++)
  {
    const InputConfig& cfg = (*inputs)[idx];
    const AVMediaType type = m_desc.inputs[idx].type;
    if(cfg.type != type)
      return fail("input type mismatch", 0);

    Input in;
    in.config = cfg;
    const std::string name = "lavfi_src_" + std::to_string(idx);
    if(type == AVMEDIA_TYPE_AUDIO)
    {
      char args[256];
      std::snprintf(
          args, sizeof(args), "sample_rate=%d:sample_fmt=fltp:channel_layout=%s:time_base=1/%d",
          cfg.audio.sample_rate, channelLayoutString(cfg.audio.channels).c_str(),
          cfg.audio.sample_rate);
      ret = avfilter_graph_create_filter(
          &in.src, avfilter_get_by_name("abuffer"), name.c_str(), args, nullptr, m_graph);
      if(ret < 0)
        return fail("could not create the audio source", ret);
    }
    else
    {
      in.src = avfilter_graph_alloc_filter(m_graph, avfilter_get_by_name("buffer"), name.c_str());
      if(!in.src)
        return fail("could not create the video source", 0);
      AVBufferSrcParameters* par = av_buffersrc_parameters_alloc();
      par->format = cfg.video.format;
      par->width = cfg.video.width;
      par->height = cfg.video.height;
      par->time_base = cfg.video.time_base;
      par->frame_rate = cfg.video.frame_rate;
      par->sample_aspect_ratio = AVRational{1, 1};
      if(cfg.video.hw_frames_ctx)
        par->hw_frames_ctx = av_buffer_ref(cfg.video.hw_frames_ctx);
      ret = av_buffersrc_parameters_set(in.src, par);
      av_buffer_unref(&par->hw_frames_ctx);
      av_freep(&par);
      if(ret < 0)
        return fail("could not set the video source parameters", ret);
      if(hw_device && !in.src->hw_device_ctx)
        in.src->hw_device_ctx = av_buffer_ref(hw_device);
      if((ret = avfilter_init_str(in.src, nullptr)) < 0)
        return fail("could not initialise the video source", ret);
    }
    if((ret = avfilter_link(in.src, 0, io->filter_ctx, io->pad_idx)) < 0)
      return fail("could not link a source", ret);
    in.frame = av_frame_alloc();
    m_inputs.push_back(in);
  }

  // --- sinks -----------------------------------------------------------------
  idx = 0;
  for(auto* io = open_outputs; io; io = io->next, idx++)
  {
    Output out;
    out.type = m_desc.outputs[idx].type;
    const std::string name = "lavfi_sink_" + std::to_string(idx);
    const bool audio = out.type == AVMEDIA_TYPE_AUDIO;
    out.sink = avfilter_graph_alloc_filter(
        m_graph, avfilter_get_by_name(audio ? "abuffersink" : "buffersink"), name.c_str());
    if(!out.sink)
      return fail("could not create a sink", 0);

    if(audio)
    {
#if LAVFI_HAS_NEW_SINK_OPTS
      av_opt_set(out.sink, "sample_formats", "fltp", AV_OPT_SEARCH_CHILDREN);
      if(sinks->sample_rate > 0)
        av_opt_set(
            out.sink, "samplerates", std::to_string(sinks->sample_rate).c_str(),
            AV_OPT_SEARCH_CHILDREN);
#else
      const AVSampleFormat fmts[] = {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE};
      av_opt_set_int_list(out.sink, "sample_fmts", fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
      if(sinks->sample_rate > 0)
      {
        const int rates[] = {sinks->sample_rate, -1};
        av_opt_set_int_list(out.sink, "sample_rates", rates, -1, AV_OPT_SEARCH_CHILDREN);
      }
#endif
    }
    else if(!sinks->pix_fmts.empty())
    {
#if LAVFI_HAS_NEW_SINK_OPTS
      std::string list;
      for(auto f : sinks->pix_fmts)
      {
        if(!list.empty())
          list += '|';
        list += av_get_pix_fmt_name(f);
      }
      av_opt_set(out.sink, "pixel_formats", list.c_str(), AV_OPT_SEARCH_CHILDREN);
#else
      std::vector<AVPixelFormat> fmts = sinks->pix_fmts;
      fmts.push_back(AV_PIX_FMT_NONE);
      av_opt_set_int_list(out.sink, "pix_fmts", fmts.data(), AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
#endif
    }
    if((ret = avfilter_init_str(out.sink, nullptr)) < 0)
      return fail("could not initialise a sink", ret);
    if((ret = avfilter_link(io->filter_ctx, io->pad_idx, out.sink, 0)) < 0)
      return fail("could not link a sink", ret);
    out.frame = av_frame_alloc();
    m_outputs.push_back(out);
  }

  if((ret = avfilter_graph_config(m_graph, nullptr)) < 0)
    return fail("could not configure the graph", ret);

  // Re-collect: config may have inserted auto-conversion filters and
  // resolved formats; the options of the user's filters are unchanged.
  collectDescription();
  return true;
}

void Graph::collectDescription()
{
  m_desc.options.clear();
  m_desc.filters.clear();
  for(unsigned i = 0; i < m_graph->nb_filters; i++)
  {
    AVFilterContext* f = m_graph->filters[i];
    if(!f || !f->name)
      continue;
    const std::string fname = f->name;
    // Our own sources/sinks and the format-negotiation helpers lavfi inserts
    // are not the user's business.
    if(fname.rfind("lavfi_", 0) == 0 || fname.rfind("auto_", 0) == 0)
      continue;
    m_desc.filters.push_back(fname);

    if(!f->priv || !f->filter->priv_class)
      continue;
    void* obj = f->priv;
    // Pass 1: constants, grouped by unit.
    std::vector<std::pair<std::string, OptionConst>> consts;
    for(const AVOption* o = av_opt_next(obj, nullptr); o; o = av_opt_next(obj, o))
    {
      // Constants keep their value in .i64 (libavutil/opt.c), whatever the
      // type of the option they belong to.
      if(o->type == AV_OPT_TYPE_CONST && o->unit)
        consts.push_back(
            {o->unit, {o->name, double(o->default_val.i64), o->help ? o->help : ""}});
    }
    // Pass 2: the options themselves. Skip aliases: lavfi lists the same
    // storage under several names (e.g. "sigma" and "s"); keep the first.
    std::vector<int> seenOffsets;
    for(const AVOption* o = av_opt_next(obj, nullptr); o; o = av_opt_next(obj, o))
    {
      if(o->type == AV_OPT_TYPE_CONST)
        continue;
      if(!(o->flags & AV_OPT_FLAG_FILTERING_PARAM))
        continue;
      if(std::find(seenOffsets.begin(), seenOffsets.end(), o->offset) != seenOffsets.end())
        continue;
      seenOffsets.push_back(o->offset);

      OptionInfo info;
      info.filter = fname;
      info.name = o->name;
      info.help = o->help ? o->help : "";
      info.type = o->type;
      info.min = o->min;
      info.max = o->max;
      info.runtime = (o->flags & AV_OPT_FLAG_RUNTIME_PARAM) != 0;
      // The instance's CURRENT value (the graph string's options have been
      // applied): that is what the control must start from. av_opt_get*
      // handle every option type, including the ones newer FFmpegs add,
      // without reading the default_val union by hand.
      {
        double d{};
        if(av_opt_get_double(obj, o->name, 0, &d) >= 0 && std::isfinite(d))
          info.def = d;
        uint8_t* str = nullptr;
        if(av_opt_get(obj, o->name, 0, &str) >= 0 && str)
        {
          info.def_str = reinterpret_cast<const char*>(str);
          av_free(str);
        }
      }
      if(o->unit)
        for(auto& [unit, c] : consts)
          if(unit == o->unit)
            info.consts.push_back(c);
      m_desc.options.push_back(std::move(info));
    }
  }
}

AVMediaType Graph::inputType(int i) const noexcept
{
  return i >= 0 && i < int(m_desc.inputs.size()) ? m_desc.inputs[i].type : AVMEDIA_TYPE_UNKNOWN;
}

AVMediaType Graph::outputType(int o) const noexcept
{
  return o >= 0 && o < int(m_outputs.size()) ? m_outputs[o].type : AVMEDIA_TYPE_UNKNOWN;
}

// ---------------------------------------------------------------------------
//  audio
// ---------------------------------------------------------------------------

bool Graph::pushAudio(int i, const float* const* planes, int channels, int frames, int64_t pts)
{
  if(i < 0 || i >= int(m_inputs.size()) || frames <= 0)
    return false;
  Input& in = m_inputs[i];
  if(in.config.type != AVMEDIA_TYPE_AUDIO || channels != in.config.audio.channels)
    return false;

  AVFrame* f = in.frame;
  av_frame_unref(f);
  f->format = AV_SAMPLE_FMT_FLTP;
  f->sample_rate = in.config.audio.sample_rate;
  f->nb_samples = frames;
  f->pts = pts;
  av_channel_layout_default(&f->ch_layout, channels);
  if(channels <= AV_NUM_DATA_POINTERS)
  {
    // Planes from a pool sized for the largest tick seen: after warm-up no
    // allocation on the audio thread. The pool recycles a plane once the
    // filters drop their reference to it.
    if(!in.pool || in.poolFrames < frames)
    {
      av_buffer_pool_uninit(&in.pool);
      // Planes padded like FFmpeg's own (FFALIGN(nb_samples, 32) plus the
      // input padding): SIMD filters may read up to that.
      in.poolFrames = FFALIGN(std::max(frames, std::max(in.poolFrames * 2, 4096)), 32);
      in.pool = av_buffer_pool_init(
          in.poolFrames * sizeof(float) + AV_INPUT_BUFFER_PADDING_SIZE, nullptr);
      if(!in.pool)
        return false;
    }
    for(int c = 0; c < channels; c++)
    {
      f->buf[c] = av_buffer_pool_get(in.pool);
      if(!f->buf[c])
      {
        av_frame_unref(f);
        return false;
      }
      f->data[c] = f->buf[c]->data;
    }
    f->linesize[0] = int(frames * sizeof(float));
    f->extended_data = f->data;
  }
  else if(av_frame_get_buffer(f, 0) < 0)
    return false;
  for(int c = 0; c < channels; c++)
    std::memcpy(f->data[c], planes[c], sizeof(float) * frames);
  // The source takes the references; f is left allocated but empty.
  return av_buffersrc_add_frame(in.src, f) >= 0;
}

int Graph::pullAudio(int o, float* const* planes, int channels, int frames)
{
  if(o < 0 || o >= int(m_outputs.size()) || frames <= 0)
    return -1;
  Output& out = m_outputs[o];
  if(out.type != AVMEDIA_TYPE_AUDIO)
    return -1;

  AVFrame* f = out.frame;
  av_frame_unref(f);
  const int ret = av_buffersink_get_samples(out.sink, f, frames);
  if(ret == AVERROR(EAGAIN))
    return 0;
  if(ret == AVERROR_EOF)
  {
    m_eof = true;
    return 0;
  }
  if(ret < 0)
    return -1;
  const int n = std::min(f->nb_samples, frames);
  const int ch = std::min(channels, f->ch_layout.nb_channels);
  for(int c = 0; c < ch; c++)
    std::memcpy(planes[c], f->data[c], sizeof(float) * n);
  for(int c = ch; c < channels; c++)
    std::memset(planes[c], 0, sizeof(float) * n);
  collectMetadata(out, *f);
  av_frame_unref(f);
  return n;
}

int Graph::outputChannels(int o) const noexcept
{
  if(o < 0 || o >= int(m_outputs.size()) || m_outputs[o].type != AVMEDIA_TYPE_AUDIO)
    return 0;
  return av_buffersink_get_channels(m_outputs[o].sink);
}

int Graph::outputSampleRate(int o) const noexcept
{
  if(o < 0 || o >= int(m_outputs.size()) || m_outputs[o].type != AVMEDIA_TYPE_AUDIO)
    return 0;
  return av_buffersink_get_sample_rate(m_outputs[o].sink);
}

// ---------------------------------------------------------------------------
//  video
// ---------------------------------------------------------------------------

bool Graph::pushVideo(int i, AVFrame* frame)
{
  if(i < 0 || i >= int(m_inputs.size()) || !frame)
    return false;
  Input& in = m_inputs[i];
  if(in.config.type != AVMEDIA_TYPE_VIDEO)
    return false;
  return av_buffersrc_add_frame_flags(in.src, frame, AV_BUFFERSRC_FLAG_KEEP_REF) >= 0;
}

AVFrame* Graph::pullVideo(int o)
{
  if(o < 0 || o >= int(m_outputs.size()))
    return nullptr;
  Output& out = m_outputs[o];
  if(out.type != AVMEDIA_TYPE_VIDEO)
    return nullptr;
  AVFrame* f = av_frame_alloc();
  const int ret = av_buffersink_get_frame(out.sink, f);
  if(ret < 0)
  {
    if(ret == AVERROR_EOF)
      m_eof = true;
    av_frame_free(&f);
    return nullptr;
  }
  collectMetadata(out, *f);
  return f;
}

int Graph::discardPending(int o)
{
  if(o < 0 || o >= int(m_outputs.size()))
    return 0;
  Output& out = m_outputs[o];
  AVFrame* f = out.frame ? out.frame : (out.frame = av_frame_alloc());
  int n = 0;
  // Bounded twice: NO_REQUEST means nothing new is produced, and the cap
  // guards against a sink that answers anyway.
  while(n < 64)
  {
    av_frame_unref(f);
    const int ret = av_buffersink_get_frame_flags(out.sink, f, AV_BUFFERSINK_FLAG_NO_REQUEST);
    if(ret < 0)
      break;
    n++;
  }
  av_frame_unref(f);
  return n;
}

int Graph::outputWidth(int o) const noexcept
{
  return o >= 0 && o < int(m_outputs.size()) && m_outputs[o].type == AVMEDIA_TYPE_VIDEO
             ? av_buffersink_get_w(m_outputs[o].sink)
             : 0;
}

int Graph::outputHeight(int o) const noexcept
{
  return o >= 0 && o < int(m_outputs.size()) && m_outputs[o].type == AVMEDIA_TYPE_VIDEO
             ? av_buffersink_get_h(m_outputs[o].sink)
             : 0;
}

AVPixelFormat Graph::outputPixelFormat(int o) const noexcept
{
  return o >= 0 && o < int(m_outputs.size()) && m_outputs[o].type == AVMEDIA_TYPE_VIDEO
             ? AVPixelFormat(av_buffersink_get_format(m_outputs[o].sink))
             : AV_PIX_FMT_NONE;
}

AVBufferRef* Graph::outputHwFramesContext(int o) const noexcept
{
  return o >= 0 && o < int(m_outputs.size()) && m_outputs[o].type == AVMEDIA_TYPE_VIDEO
             ? av_buffersink_get_hw_frames_ctx(m_outputs[o].sink)
             : nullptr;
}

// ---------------------------------------------------------------------------

bool Graph::sendCommand(
    const std::string& target, const std::string& cmd, const std::string& arg,
    std::string* response)
{
  if(!m_graph)
    return false;
  char res[256]{};
  const int ret = avfilter_graph_send_command(
      m_graph, target.c_str(), cmd.c_str(), arg.c_str(), res, sizeof(res), 0);
  if(response)
    *response = res;
  return ret >= 0;
}

void Graph::collectMetadata(Output& out, const AVFrame& f)
{
  out.metadata.clear();
  if(!f.metadata)
    return;
  const AVDictionaryEntry* e = nullptr;
  while((e = av_dict_iterate(f.metadata, e)))
  {
    if(e->key && std::strncmp(e->key, "lavfi.", 6) == 0)
      out.metadata.emplace_back(e->key + 6, e->value ? e->value : "");
  }
}

const std::vector<std::pair<std::string, std::string>>&
Graph::lastMetadata(int o) const noexcept
{
  static const std::vector<std::pair<std::string, std::string>> empty;
  return o >= 0 && o < int(m_outputs.size()) ? m_outputs[o].metadata : empty;
}

std::string Graph::takeLog()
{
  std::lock_guard lock{LogRegistry::instance().mutex};
  std::string s = std::move(m_log);
  m_log.clear();
  if(!m_logLine.empty())
  {
    s += m_logLine;
    m_logLine.clear();
  }
  return s;
}

} // namespace Lavfi
