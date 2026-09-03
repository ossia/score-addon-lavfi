#include "AudioNode.hpp"

#include <ossia/dataflow/exec_state_facade.hpp>
#include <ossia/network/value/value_conversion.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace Lavfi
{
std::string commandArgument(const ossia::value& v)
{
  switch(v.get_type())
  {
    case ossia::val_type::FLOAT: {
      // std::to_chars: shortest round-trip text, locale independent.
      char buf[64];
      auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), *v.target<float>());
      return ec == std::errc{} ? std::string(buf, p) : std::to_string(*v.target<float>());
    }
    case ossia::val_type::INT:
      return std::to_string(*v.target<int>());
    case ossia::val_type::BOOL:
      return *v.target<bool>() ? "1" : "0";
    case ossia::val_type::STRING:
      return *v.target<std::string>();
    case ossia::val_type::VEC4F:
      // The colour controls: score keeps RGBA in 0-1, lavfi wants 0xRRGGBBAA.
      // Anything else that is four floats reads as a colour too, and there is
      // no lavfi option taking four numbers that is not one.
      if(const auto* v4 = v.target<ossia::vec4f>())
        return Lavfi::formatColor(v4->data());
      [[fallthrough]];
    case ossia::val_type::VEC2F:
    case ossia::val_type::VEC3F:
    case ossia::val_type::LIST: {
      // "WxH", "num/den" and friends are strings in lavfi; a list becomes the
      // "a|b|c" form most array options accept.
      std::string s;
      for(const auto& e : ossia::convert<std::vector<ossia::value>>(v))
      {
        if(!s.empty())
          s += '|';
        s += commandArgument(e);
      }
      return s;
    }
    default:
      return ossia::convert<std::string>(v);
  }
}

/// Is this value worth handing to libavfilter?
///
/// Only what changed, and never an empty string: a filter re-reads its whole
/// configuration when it takes a command, so sending "" for the options that
/// happen to be empty (curves' `psfile`, its `master` curve) clears the ones
/// the graph text had set and the filter quietly turns into a pass-through --
/// and a file option answers an empty name with a read error.
bool worthSending(const Lavfi::OptionInfo& o, const std::string& arg)
{
  if(arg.empty())
    return false;
  return true;
}

audio_node::audio_node(
    std::string script, Lavfi::Description desc, std::vector<Lavfi::OptionInfo> controls,
    int sampleRate)
    : m_script{std::move(script)}
    , m_desc{std::move(desc)}
    , m_controls{std::move(controls)}
    , m_rate{sampleRate}
{
  for(const auto& pad : m_desc.inputs)
    if(pad.type == AVMEDIA_TYPE_AUDIO)
    {
      m_inlets.push_back(new ossia::audio_inlet);
      m_nAudioIn++;
    }
  for(std::size_t i = 0; i < m_controls.size(); i++)
    m_inlets.push_back(new ossia::value_inlet);
  for(const auto& pad : m_desc.outputs)
    if(pad.type == AVMEDIA_TYPE_AUDIO)
    {
      m_outlets.push_back(new ossia::audio_outlet);
      m_nAudioOut++;
    }
  m_outlets.push_back(new ossia::value_outlet);
  m_inChannels.assign(m_nAudioIn, 0);
}

audio_node::~audio_node() = default;

void audio_node::setProgram(
    std::string script, Lavfi::Description desc, std::vector<Lavfi::OptionInfo> controls)
{
  // Only the program can change through this path: the port layout is fixed
  // at construction, and the executor recreates the node when it changes.
  m_script = std::move(script);
  m_desc = std::move(desc);
  m_controls = std::move(controls);
  m_needsInit = true;
  m_failed = false;
}

std::string audio_node::lastError() const
{
  return m_error;
}

bool audio_node::rebuild(int sampleRate, const std::vector<int>& inChannels)
{
  m_rate = sampleRate;
  m_inChannels = inChannels;
  m_pos = 0;

  std::vector<Lavfi::InputConfig> inputs;
  int a = 0;
  for(const auto& pad : m_desc.inputs)
  {
    Lavfi::InputConfig cfg;
    cfg.type = pad.type;
    if(pad.type == AVMEDIA_TYPE_AUDIO)
    {
      cfg.audio.sample_rate = sampleRate;
      cfg.audio.channels = std::max(1, inChannels[a++]);
    }
    inputs.push_back(cfg);
  }
  Lavfi::SinkConfig sinks;
  sinks.sample_rate = sampleRate;
  sinks.threads = 1;

  auto g = std::make_unique<Lavfi::Graph>();
  std::string err;
  if(!g->init(m_script, inputs, sinks, nullptr, err, buildTimeOptions()))
  {
    m_error = err;
    m_graph.reset();
    return false;
  }
  m_error.clear();
  m_graph = std::move(g);

  // Push the current control values so a graph rebuilt mid-play does not
  // revert to the option defaults. The build-time ones are already in.
  for(std::size_t k = 0; k < m_controls.size(); k++)
  {
    if(!m_controls[k].runtime)
      continue;
    auto& port = m_inlets[m_nAudioIn + k]->cast<ossia::value_port>();
    auto& data = port.get_data();
    if(!data.empty())
      m_graph->sendCommand(
          m_controls[k].filter, m_controls[k].name, commandArgument(data.back().value));
  }
  return true;
}

std::vector<Lavfi::OptionValue> audio_node::buildTimeOptions() const
{
  std::vector<Lavfi::OptionValue> opts;
  for(std::size_t k = 0; k < m_controls.size(); k++)
  {
    if(m_controls[k].runtime)
      continue;
    if(k < m_optionValues.size() && !m_optionValues[k].empty())
      opts.push_back({m_controls[k].filter, m_controls[k].name, m_optionValues[k]});
  }
  return opts;
}

void audio_node::silence(ossia::exec_state_facade st, int64_t first, int64_t n)
{
  for(int o = 0; o < m_nAudioOut; o++)
  {
    auto& out = m_outlets[o]->cast<ossia::audio_port>();
    if(out.channels() == 0)
      out.set_channels(1);
    for(std::size_t c = 0; c < out.channels(); c++)
    {
      auto& ch = out.channel(c);
      ch.resize(st.bufferSize());
      std::fill(ch.begin() + first, ch.begin() + std::min<int64_t>(first + n, ch.size()), 0.);
    }
  }
}

void audio_node::run(const ossia::token_request& t, ossia::exec_state_facade st) noexcept
{
  const auto [first_pos, N] = st.timings(t);
  if(N <= 0)
    return;
  const int rate = st.sampleRate();

  // --- (re)build when the inputs' shape changed ------------------------------
  bool needsInit = m_needsInit || rate != m_rate || (!m_graph && !m_failed);
  std::vector<int>& chans = m_inChannels;
  for(int i = 0; i < m_nAudioIn; i++)
  {
    auto& in = m_inlets[i]->cast<ossia::audio_port>();
    int ch = int(in.channels());
    if(ch == 0)
      ch = std::max(1, chans[i]); // unconnected: keep the last known layout
    if(ch != chans[i])
    {
      needsInit = true;
      chans[i] = ch;
    }
  }
  if(needsInit)
  {
    m_needsInit = false;
    m_failed = !rebuild(rate, chans);
  }
  if(m_failed || !m_graph)
  {
    // A program that does not build stays silent until it changes (or the
    // inputs change shape); no parse + configure on every tick.
    silence(st, first_pos, N);
    return;
  }

  // --- options ---------------------------------------------------------------
  // Runtime ones go straight in. The others are only read when a filter is
  // initialised, so a change there means building the graph again: on the next
  // tick, once, however many of them moved.
  m_optionValues.resize(m_controls.size());
  bool rebuildForOptions = false;
  for(std::size_t k = 0; k < m_controls.size(); k++)
  {
    auto& port = m_inlets[m_nAudioIn + k]->cast<ossia::value_port>();
    auto& data = port.get_data();
    if(data.empty())
      continue;
    const auto arg = commandArgument(data.back().value);
    if(!worthSending(m_controls[k], arg) || m_optionValues[k] == arg)
      continue;
    m_optionValues[k] = arg;
    if(m_controls[k].runtime)
      m_graph->sendCommand(m_controls[k].filter, m_controls[k].name, arg);
    else
      rebuildForOptions = true;
  }
  if(rebuildForOptions)
  {
    m_failed = !rebuild(rate, chans);
    if(m_failed || !m_graph)
    {
      silence(st, first_pos, N);
      return;
    }
  }

  // --- push this tick's input samples -------------------------------------------
  for(int i = 0; i < m_nAudioIn; i++)
  {
    auto& in = m_inlets[i]->cast<ossia::audio_port>();
    const int ch = chans[i];
    if(int(m_scratch.size()) < ch)
      m_scratch.resize(ch);
    m_inPlanes.resize(ch);
    for(int c = 0; c < ch; c++)
    {
      auto& s = m_scratch[c];
      s.resize(N);
      if(c < int(in.channels()))
      {
        const auto& src = in.channel(c);
        for(int64_t k = 0; k < N; k++)
        {
          const int64_t p = first_pos + k;
          s[k] = p < int64_t(src.size()) ? float(src[p]) : 0.f;
        }
      }
      else
        std::fill(s.begin(), s.end(), 0.f);
      m_inPlanes[c] = s.data();
    }
    m_graph->pushAudio(i, m_inPlanes.data(), ch, int(N), m_pos);
  }
  m_pos += N;

  // --- pull exactly N samples per output ---------------------------------------
  for(int o = 0; o < m_nAudioOut; o++)
  {
    auto& out = m_outlets[o]->cast<ossia::audio_port>();
    const int ch = std::max(1, m_graph->outputChannels(o));
    out.set_channels(ch);
    if(int(m_scratch.size()) < ch)
      m_scratch.resize(ch);
    m_outPlanes.resize(ch);
    for(int c = 0; c < ch; c++)
    {
      m_scratch[c].resize(N);
      m_outPlanes[c] = m_scratch[c].data();
    }
    const int got = std::max(0, m_graph->pullAudio(o, m_outPlanes.data(), ch, int(N)));
    for(int c = 0; c < ch; c++)
    {
      auto& dst = out.channel(c);
      dst.resize(st.bufferSize());
      const auto* src = m_outPlanes[c];
      for(int64_t k = 0; k < N; k++)
        dst[first_pos + k] = k < got ? double(src[k]) : 0.;
    }
  }

  // --- metadata ----------------------------------------------------------------
  {
    // A map: a receiver asks for "lavfi.r128.M" by name rather than counting
    // into a list of pairs.
    ossia::value_map_type map;
    for(int o = 0; o < m_nAudioOut; o++)
      for(const auto& [k, v] : m_graph->lastMetadata(o))
      {
        // Numeric where it parses as such, string otherwise.
        double d{};
        auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), d);
        ossia::value val = (ec == std::errc{} && p == v.data() + v.size()) ? ossia::value{float(d)}
                                                                              : ossia::value{v};
        map.emplace_back(k, std::move(val));
      }
    if(!map.empty())
      m_outlets.back()->cast<ossia::value_port>().write_value(std::move(map), 0);
  }
}
}
