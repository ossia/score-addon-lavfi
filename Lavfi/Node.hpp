#pragma once
#include <Lavfi/Export.hpp>
#include <Gfx/GfxExecNode.hpp>
#include <Gfx/Graph/Node.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/RenderList.hpp>

#include <Lavfi/Core/Graph.hpp>
#include <Video/VideoInterface.hpp>

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <mutex>
#include <string>
#include <vector>

namespace score::gfx
{
class GPUVideoDecoder;
}

namespace Lavfi
{
class VulkanTransport;

struct MetadataMailbox
{
  std::mutex mutex;
  ossia::value value;
  bool changed{};
  void post(ossia::value v)
  {
    std::lock_guard lock{mutex};
    value = std::move(v);
    changed = true;
  }
  bool take(ossia::value& out)
  {
    std::lock_guard lock{mutex};
    if(!changed)
      return false;
    changed = false;
    out = std::move(value);
    return true;
  }
};

/**
 * @brief Render-thread node for graphs with video pads.
 *
 * Ports mirror the model: one Image input per texture inlet, one Audio input
 * per audio inlet, one Float input per control inlet; one Image output per
 * texture outlet. Control values and audio arrive as messages from the
 * execution node and are handed to the renderer at its next update; the
 * lavfi.* metadata of the sink frames goes back to the execution node's
 * Metadata outlet through the execution command queue.
 *
 * Frame transport, chosen per renderer at init (SCORE_LAVFI_TRANSPORT=auto|
 * vulkan|cpu overrides, SCORE_LAVFI_DEBUG=1 logs the choice):
 *
 *  - Vulkan (Lavfi::VulkanTransport): on the Vulkan backend with score's shared
 *    VkDevice, the input render targets are FFmpeg pool images and the sink
 *    frame is sampled directly. Zero copies; the graph's filters must accept
 *    Vulkan frames (`*_vulkan`, `libplacebo`, `hwupload`/`hwdownload` inside
 *    the string for CPU stages). If the graph refuses Vulkan frames the
 *    renderer falls back to the CPU transport.
 *
 *  - CPU: each texture input is rendered into an RGBA8 render target, read back
 *    asynchronously (one frame of latency, no GPU stall), copied into an
 *    AVFrame and pushed; the sink frame, in whichever pixel format the graph
 *    settled on among those score's GPU decoders upload, goes through
 *    score::gfx::createGPUVideoDecoder (GPU colour conversion on upload). The
 *    graph still gets a hardware device (Vulkan, D3D11, VideoToolbox or CUDA,
 *    Lavfi::preferredDeviceForRhi) so `hwupload,...,hwdownload` stages work on
 *    every backend.
 */
class SCORE_ADDON_LAVFI_EXPORT GfxNode final : public score::gfx::NodeModel
{
public:
  struct Program
  {
    std::string script;
    Lavfi::Description desc;
    std::vector<Lavfi::OptionInfo> controls;
    int sampleRate{48000};
    /// Latest lavfi.* metadata of the sink frame, render thread -> execution
    /// thread. The execution node moves it into its Metadata control-out at
    /// its next tick (Gfx::exec_control is not thread-safe and the execution
    /// command queue is single-producer, so neither is written from here).
    std::shared_ptr<MetadataMailbox> metadata;
  };

  explicit GfxNode(Program program);
  ~GfxNode() override;

  score::gfx::NodeRenderer* createRenderer(score::gfx::RenderList& r) const noexcept override;

  void process(int32_t port, const ossia::value& v) override;
  void process(int32_t port, const ossia::audio_vector& v) override;

  /// Index of a port in the exec node's inlet list -> which control (or -1).
  int controlIndex(int32_t port) const noexcept;

  const Program& program() const noexcept { return m_program; }
  std::vector<std::pair<int, ossia::value>> takePendingControls();
  std::vector<std::pair<int, ossia::audio_vector>> takePendingAudio();

private:
  friend class GfxRenderer;
  Program m_program;
  std::vector<int> m_portToControl; ///< exec inlet index -> control index
  std::vector<int> m_portToAudio;   ///< exec inlet index -> audio input index
  mutable std::mutex m_mutex;
  std::vector<std::pair<int, ossia::value>> m_pendingControls;
  std::vector<std::pair<int, ossia::audio_vector>> m_pendingAudio;
};

class GfxRenderer final : public score::gfx::NodeRenderer
{
public:
  explicit GfxRenderer(const GfxNode& node) noexcept;
  ~GfxRenderer() override;

  enum class Transport
  {
    None,
    Cpu,
    Vulkan
  };
  Transport transport() const noexcept { return m_transport; }

  // NodeRenderer lifecycle. RenderList drives both the whole-renderer pair
  // (init/release) and the surgical one (initState/releaseState +
  // addOutputPass/removeOutputPass) when an input's spec changes.
  score::gfx::TextureRenderTarget renderTargetForInput(const score::gfx::Port& p) override;
  void inputAboutToFinish(
      score::gfx::RenderList& renderer, const score::gfx::Port& p,
      QRhiResourceUpdateBatch*& res) override;
  void init(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res) override;
  void release(score::gfx::RenderList& r) override;
  void initState(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res) override;
  void releaseState(score::gfx::RenderList& renderer) override;
  void addOutputPass(
      score::gfx::RenderList& renderer, score::gfx::Edge& edge,
      QRhiResourceUpdateBatch& res) override;
  void removeOutputPass(score::gfx::RenderList& renderer, score::gfx::Edge& edge) override;
  bool hasOutputPassForEdge(score::gfx::Edge& edge) const override;
  void update(
      score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res,
      score::gfx::Edge* edge) override;
  void runRenderPass(
      score::gfx::RenderList& renderer, QRhiCommandBuffer& cb,
      score::gfx::Edge& edge) override;

private:
  struct Input
  {
    const score::gfx::Port* port{};
    int graphInput{};  ///< index among the graph's inputs
    int vkInput{-1};   ///< index in the Vulkan transport
    QSize size;
    // CPU transport
    score::gfx::TextureRenderTarget rt;
    QRhiReadbackResult readback;
  };

  Transport chooseTransport(score::gfx::RenderList& renderer) const;
  bool setupTransport(score::gfx::RenderList& renderer, Transport t);
  void releaseTransport(score::gfx::RenderList& renderer);
  bool buildGraph(score::gfx::RenderList& renderer);
  bool setupOutputPass(score::gfx::RenderList& renderer);
  bool setupDecoder(score::gfx::RenderList& renderer, AVPixelFormat fmt, int w, int h);
  void rebuildPasses(score::gfx::RenderList& renderer);
  void pushReadbacks(score::gfx::RenderList& renderer);
  void pushVulkanInputs();
  void pushAudio();
  void applyControls();
  /// The controls libavfilter only reads when it initialises a filter.
  std::vector<Lavfi::OptionValue> buildTimeOptions() const;
  void publishMetadata();
  void drainUnusedOutputs();
  bool currentShaders(
      const QShader*& vs, const QShader*& fs, std::span<const score::gfx::Sampler>& samplers) const;

  const GfxNode& lavfiNode() const noexcept { return static_cast<const GfxNode&>(node); }

  bool m_initialized{};
  int64_t m_lastFrame{-1};
  Transport m_transport{Transport::None};
  std::unique_ptr<Lavfi::Graph> m_graph;
  bool m_graphFailed{}; ///< no retry every frame; cleared by (re)initState
  std::string m_error;
  std::vector<Input> m_inputs;
  std::vector<int> m_audioChannels; ///< per audio input; rebuild when a message disagrees
  bool m_rebuildGraph{};
  /// Last value seen for each control, as libavfilter spells it; indices match
  /// the program's control list.
  std::vector<std::string> m_optionValues;
  int64_t m_frameCounter{};
  bool m_flipY{};
  AVBufferRef* m_hwDevice{}; ///< handed to the graph's filters (any transport)
  AVHWDeviceType m_hwDeviceType{AV_HWDEVICE_TYPE_NONE};
  bool m_vulkanRefused{}; ///< the graph would not take Vulkan frames: stay on CPU

  // Common GPU state
  score::gfx::MeshBuffers m_meshBuffer;
  score::gfx::PassMap m_p;
  QRhiBuffer* m_processUBO{};
  QRhiBuffer* m_materialUBO{};
  struct Material
  {
    float scale_w{1.f}, scale_h{1.f};
    float tex_w{}, tex_h{};
  };

  // Vulkan transport
  std::unique_ptr<VulkanTransport> m_vk;
  QRhiSampler* m_vkSampler{};
  std::vector<score::gfx::Sampler> m_vkSamplers;
  QShader m_vkVertex, m_vkFragment;

  // CPU transport output: score's video upload path.
  Video::ImageFormat m_format{};
  std::unique_ptr<score::gfx::GPUVideoDecoder> m_decoder;
  std::pair<QShader, QShader> m_shaders;
  // Frames handed to the decoder stay referenced until the upload that
  // wraps their bytes has been committed: a 3-slot ring covers that.
  AVFrame* m_frames[3]{};
  int m_frameSlot{};
  AVFrame* m_inputFrame{}; ///< Scratch frame the readbacks are copied into.
  int64_t m_audioPos{};
  bool m_hasOutput{};
};
}
