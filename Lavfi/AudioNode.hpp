#pragma once
#include <Lavfi/Core/Graph.hpp>

#include <ossia/dataflow/graph_node.hpp>
#include <ossia/dataflow/port.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace Lavfi
{
/**
 * @brief Execution node for graphs with audio pads only.
 *
 * Runs the libavfilter graph inside the audio tick: every input pad gets the
 * tick's samples as one FLTP frame, every output pad is pulled for exactly the
 * tick's sample count (av_buffersink_get_samples), so score's variable tick
 * sizes are respected. A filter with lookahead (afir, loudnorm, atempo...)
 * yields silence until it has enough input; the shortfall is not hidden.
 *
 * Inlets : one audio inlet per input pad, then one value inlet per runtime
 *          option (forwarded as avfilter_process_command).
 * Outlets: one audio outlet per output pad, then one value outlet with the
 *          lavfi.* metadata of the last pulled frame (as a list of [key, value]).
 *
 * The graph is (re)built inside run() when the script, the sample rate or an
 * input's channel count changes. That allocates, in the audio thread: it is a
 * one-off per change, the same trade-off Faust/VST hosting makes here.
 */
class audio_node final : public ossia::graph_node
{
public:
  audio_node(
      std::string script, Lavfi::Description desc, std::vector<Lavfi::OptionInfo> controls,
      int sampleRate);
  ~audio_node() override;

  std::string label() const noexcept override { return "lavfi"; }

  void run(const ossia::token_request& t, ossia::exec_state_facade st) noexcept override;

  /// Called from the execution thread (through in_exec): takes effect at the next tick.
  void setProgram(
      std::string script, Lavfi::Description desc, std::vector<Lavfi::OptionInfo> controls);

  /// Log lines of the last (re)build, for diagnostics.
  std::string lastError() const;

  /// Bumped by the executor on every (re)wiring of the controls; queued
  /// control writes carry the generation they were issued for.
  int generation{};

private:
  bool rebuild(int sampleRate, const std::vector<int>& inChannels);
  void silence(ossia::exec_state_facade st, int64_t first, int64_t n);

  std::string m_script;
  Lavfi::Description m_desc;
  std::vector<Lavfi::OptionInfo> m_controls;
  std::unique_ptr<Lavfi::Graph> m_graph;

  int m_nAudioIn{}, m_nAudioOut{};
  int m_rate{};
  std::vector<int> m_inChannels;
  bool m_needsInit{true};
  bool m_failed{}; ///< the current program does not build: no retry every tick
  int64_t m_pos{};
  std::string m_error;

  // Scratch planes: ossia audio is double, libavfilter wants float.
  std::vector<std::vector<float>> m_scratch;
  std::vector<const float*> m_inPlanes;
  std::vector<float*> m_outPlanes;
};

/// The textual form avfilter_process_command wants for a control's value.
std::string commandArgument(const ossia::value& v);
}
