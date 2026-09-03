#pragma once

/**
 * @file Graph.hpp
 * @brief Qt-free wrapper around an FFmpeg libavfilter graph.
 *
 * Everything the score process, the audio node and the gfx renderer need
 * from libavfilter goes through this class, so it can be exercised headless
 * (tests/GraphTest.cpp) and reasoned about without score:
 *
 *  - describe(): parse a filtergraph string on a throw-away graph and report
 *    its open input/output pads and the options of every filter instance
 *    (with their ranges, constants and whether they accept runtime commands).
 *    The process model derives its ports from this.
 *  - init(): build the real graph: one buffersrc per open input pad with the
 *    format the caller feeds, one buffersink per open output pad, an optional
 *    hardware device handed to every filter BEFORE it is initialised (this is
 *    why the segment API is used: hwupload & friends read hw_device_ctx in
 *    init), then avfilter_graph_config.
 *  - pushAudio / pullAudio, pushVideo / pullVideo: one tick or frame at a time.
 *  - sendCommand(): avfilter_graph_send_command for runtime parameters.
 *  - av_log capture: log lines emitted by this graph's filters are collected
 *    per graph (takeLog()) so a parse or configure error reaches the script
 *    editor's error pane instead of stderr.
 *
 * Threading: a Graph is used from one thread at a time (the audio thread for
 * audio graphs, the render thread for video graphs). Building one is not
 * realtime-safe; swapping a built one in is.
 */

#include <Lavfi/Export.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

namespace Lavfi
{

/// One open (unconnected) pad of the parsed graph. It becomes a score port.
struct PadInfo
{
  std::string name;      ///< Label if the user wrote one ([in]), else generated.
  AVMediaType type{};    ///< AVMEDIA_TYPE_AUDIO or AVMEDIA_TYPE_VIDEO.
  std::string filter;    ///< Instance name of the filter the pad belongs to.
  int pad{};             ///< Pad index on that filter.
};

struct OptionConst
{
  std::string name;
  double value{};
  std::string help;
};

/// One AVOption of one filter instance.
struct OptionInfo
{
  std::string filter; ///< Filter instance name, e.g. "Parsed_gblur_0" or "blur" for gblur@blur.
  std::string name;   ///< Option name, e.g. "sigma".
  std::string help;
  AVOptionType type{};
  double min{}, max{}, def{};
  std::string def_str; ///< Default for string-like options.
  bool runtime{};      ///< AV_OPT_FLAG_RUNTIME_PARAM: changeable through sendCommand.
  std::vector<OptionConst> consts; ///< Named values when the option has a unit.

  /// "filter/name": how the option is addressed in a command. Not shown.
  std::string portName() const { return filter + "/" + name; }

  /// What the port is called in score: "gblur sigma". libavfilter's instance
  /// names ("Parsed_gblur_0") and its underscores are not for reading, and a
  /// '/' in a port name reads as a path separator everywhere in score.
  std::string displayName() const;
};

/// An option set on a filter before the graph is initialised, for the options
/// libavfilter will not take at runtime: the graph is rebuilt around them.
struct OptionValue
{
  std::string filter, name, value;
};

struct SCORE_ADDON_LAVFI_EXPORT Description
{
  std::vector<PadInfo> inputs;
  std::vector<PadInfo> outputs;
  std::vector<OptionInfo> options;
  std::vector<std::string> filters; ///< Instance names, graph order.

  bool hasVideo() const noexcept;
  bool hasAudio() const noexcept;
};

struct AudioInputConfig
{
  int sample_rate{48000};
  int channels{2};
};

struct VideoInputConfig
{
  int width{}, height{};
  AVPixelFormat format{AV_PIX_FMT_RGBA};
  AVRational time_base{1, 60};
  AVRational frame_rate{60, 1};
  AVBufferRef* hw_frames_ctx{}; ///< Borrowed; ref'd by init when set (Vulkan/CUDA/... frames).
};

struct InputConfig
{
  AVMediaType type{AVMEDIA_TYPE_AUDIO};
  AudioInputConfig audio;
  VideoInputConfig video;
};

struct SinkConfig
{
  /// Pixel formats the video sinks may produce; empty = let the graph decide.
  std::vector<AVPixelFormat> pix_fmts;
  /// Sample rate the audio sinks must produce (0 = graph decides). Sample
  /// format is always FLTP: that is what the audio node reads back.
  int sample_rate{};
  /// Slice threading for CPU filters. 0 = auto (libavfilter picks), 1 = none.
  int threads{1};
};

class SCORE_ADDON_LAVFI_EXPORT Graph
{
public:
  Graph();
  ~Graph();
  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;

  /**
   * @brief Turn human-written graph text into what libavfilter accepts.
   *
   * Strips whole-line '#' comments and joins the remaining lines, inserting
   * ',' where neither side already separates them. describe() and init() both
   * run this, so a preset, a .lavfi file and text typed in the script editor
   * all behave the same. Exposed for tests and for the library file reader.
   */
  static std::string preprocess(const std::string& text);

  /**
   * @brief Parse @p text and describe it, without configuring anything.
   * @return false on parse error, with @p error filled from the log.
   */
  static bool describe(const std::string& text, Description& out, std::string& error);

  /**
   * @brief Build and configure the graph.
   * @param inputs one entry per open input pad, in the order describe() lists them.
   * @param hw_device optional AVHWDeviceContext ref; ref'd onto every filter.
   */
  /// @param options values applied to the filters before they are initialised
  ///        (what sendCommand cannot do afterwards).
  bool init(
      const std::string& text, const std::vector<InputConfig>& inputs,
      const SinkConfig& sinks, AVBufferRef* hw_device, std::string& error,
      const std::vector<OptionValue>& options = {});

  bool valid() const noexcept { return m_graph != nullptr; }
  const Description& description() const noexcept { return m_desc; }

  int inputCount() const noexcept { return int(m_inputs.size()); }
  int outputCount() const noexcept { return int(m_outputs.size()); }
  AVMediaType inputType(int i) const noexcept;
  AVMediaType outputType(int o) const noexcept;
  const InputConfig& inputConfig(int i) const noexcept { return m_inputs[i].config; }

  // ---- audio ------------------------------------------------------------
  /// Push @p frames samples of planar float audio into input @p i.
  bool pushAudio(
      int i, const float* const* planes, int channels, int frames, int64_t pts);
  /// Pull exactly @p frames samples from output @p o into planar float
  /// buffers of @p channels planes. Returns the number of frames written
  /// (0 when the graph has nothing yet, e.g. a lookahead filter), -1 on error.
  int pullAudio(int o, float* const* planes, int channels, int frames);
  int outputChannels(int o) const noexcept;
  int outputSampleRate(int o) const noexcept;

  // ---- video ------------------------------------------------------------
  /// Push a video frame (the graph takes its own reference, @p frame is untouched).
  bool pushVideo(int i, AVFrame* frame);
  /// Pull one frame from output @p o. Returns nullptr when nothing is ready
  /// (EAGAIN) or on error. The caller owns the returned frame (av_frame_free).
  AVFrame* pullVideo(int o);
  int outputWidth(int o) const noexcept;
  int outputHeight(int o) const noexcept;
  AVPixelFormat outputPixelFormat(int o) const noexcept;
  AVBufferRef* outputHwFramesContext(int o) const noexcept;
  bool eof() const noexcept { return m_eof; }
  /// Drop whatever output @p o has already produced, without asking the graph
  /// for more (AV_BUFFERSINK_FLAG_NO_REQUEST): a source-fed output would
  /// otherwise produce forever. Returns the number of frames dropped.
  int discardPending(int o);

  // ---- runtime control ---------------------------------------------------
  /// avfilter_graph_send_command(target, cmd, arg). target is a filter
  /// instance name, or "all".
  bool sendCommand(
      const std::string& target, const std::string& cmd, const std::string& arg,
      std::string* response = nullptr);

  /// Metadata ("lavfi.*" keys) carried by the last frame pulled from output @p o.
  const std::vector<std::pair<std::string, std::string>>& lastMetadata(int o) const noexcept;

  /// Log lines libavfilter emitted for this graph since the last call.
  std::string takeLog();

  AVFilterGraph* handle() const noexcept { return m_graph; }

private:
  struct Input
  {
    AVFilterContext* src{};
    InputConfig config;
    AVFrame* frame{};      ///< Scratch frame for pushAudio.
    AVBufferPool* pool{};  ///< Sample planes for pushAudio: no malloc per tick.
    int poolFrames{};      ///< Capacity of each plane, in samples.
  };
  struct Output
  {
    AVFilterContext* sink{};
    AVMediaType type{};
    AVFrame* frame{}; ///< Scratch frame for pullAudio.
    std::vector<std::pair<std::string, std::string>> metadata;
  };

  bool build(
      const std::string& text, const std::vector<InputConfig>* inputs,
      const SinkConfig* sinks, AVBufferRef* hw_device, bool configure,
      std::string& error);
  void collectDescription();
  void collectMetadata(Output& out, const AVFrame& f);
  void destroy();

  AVFilterGraph* m_graph{};
  Description m_desc;
  std::vector<Input> m_inputs;
  std::vector<Output> m_outputs;
  bool m_eof{};
  /// Option values applied at build time; see OptionValue.
  std::vector<OptionValue> m_options;
  /// Reported once: a filter emitting non-finite samples is worth saying, but
  /// not every tick.
  bool m_reportedNonFinite{};

  // av_log capture
  friend struct LogRegistry;
  std::string m_log;
  std::string m_logLine;
  int m_logPrefix{1}; ///< av_log_format_line2's persistent print_prefix
};

/// Human-readable name of an AVOptionType, for the UI.
const char* optionTypeName(AVOptionType t) noexcept;

/// True when libavfilter has @p name (e.g. "gblur_vulkan"): the process uses
/// it to report which GPU filter families the running FFmpeg was built with.
SCORE_ADDON_LAVFI_EXPORT bool hasFilter(const char* name) noexcept;

/// Parse what libavfilter accepts as a colour ("red", "#ff8000", "0xRRGGBBAA")
/// into the 0-1 RGBA score's colour control uses. False if it is not a colour.
SCORE_ADDON_LAVFI_EXPORT bool parseColor(const std::string& text, float rgba[4]) noexcept;

/// The inverse: what to hand back to libavfilter for that colour.
SCORE_ADDON_LAVFI_EXPORT std::string formatColor(const float rgba[4]);

} // namespace Lavfi
