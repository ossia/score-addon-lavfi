#pragma once
#include <Gfx/Graph/Node.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/RenderList.hpp>

#include <Lavfi/Core/Graph.hpp>
#include <Video/VideoInterface.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace score::gfx
{
class GPUVideoDecoder;
}

namespace Lavfi
{
/**
 * @brief Render-thread node for graphs with video pads.
 *
 * Ports mirror the model: one Image input per texture inlet, one Audio input
 * per audio inlet, one Float input per control inlet; one Image output per
 * texture outlet. Control values and audio arrive as messages from the
 * execution node and are handed to the renderer at its next update.
 *
 * Frame transport (chosen per renderer at init):
 *  - CPU: each texture input is rendered into an RGBA8 render target, read
 *    back asynchronously (one frame of latency, no GPU stall), copied into
 *    an AVFrame and pushed; the sink frame, in whichever pixel format the
 *    graph settled on among those score's GPU decoders can upload, goes
 *    through score::gfx::createGPUVideoDecoder, which uploads and colour-
 *    converts on the GPU. Works on every QRhi backend.
 *  - Vulkan (next phase, see PLAN.md): the render target IS an FFmpeg pool
 *    image and the sink image is sampled directly; no readback, no upload.
 */
class GfxNode final : public score::gfx::NodeModel
{
public:
  struct Program
  {
    std::string script;
    Lavfi::Description desc;
    std::vector<Lavfi::OptionInfo> controls;
  };

  explicit GfxNode(Program program);
  ~GfxNode() override;

  score::gfx::NodeRenderer* createRenderer(score::gfx::RenderList& r) const noexcept override;

  void process(int32_t port, const ossia::value& v) override;
  void process(int32_t port, const ossia::audio_vector& v) override;

  /// Index of a port in the exec node's inlet list -> which control (or -1).
  int controlIndex(int32_t port) const noexcept;

  // Read by the renderers under m_mutex.
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

class GfxRenderer final : public score::gfx::GenericNodeRenderer
{
public:
  explicit GfxRenderer(const GfxNode& node) noexcept;
  ~GfxRenderer() override;

  score::gfx::TextureRenderTarget renderTargetForInput(const score::gfx::Port& p) override;
  void inputAboutToFinish(
      score::gfx::RenderList& renderer, const score::gfx::Port& p,
      QRhiResourceUpdateBatch*& res) override;
  void init(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res) override;
  void update(
      score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res,
      score::gfx::Edge* edge) override;
  void runRenderPass(
      score::gfx::RenderList& renderer, QRhiCommandBuffer& cb,
      score::gfx::Edge& edge) override;
  void release(score::gfx::RenderList& r) override;

private:
  struct Input
  {
    const score::gfx::Port* port{};
    score::gfx::TextureRenderTarget rt;
    QRhiReadbackResult readback;
    int graphInput{}; ///< index among the graph's inputs
  };

  bool buildGraph(score::gfx::RenderList& renderer);
  bool setupDecoder(score::gfx::RenderList& renderer, AVPixelFormat fmt, int w, int h);
  void pushReadbacks(score::gfx::RenderList& renderer);
  void pushAudio();
  void applyControls();

  const GfxNode& lavfiNode() const noexcept { return static_cast<const GfxNode&>(node); }

  std::unique_ptr<Lavfi::Graph> m_graph;
  std::string m_error;
  std::vector<Input> m_inputs;
  std::vector<int> m_audioInputs; ///< graph input index per audio inlet, in exec order
  int64_t m_frameCounter{};
  bool m_flipY{};

  // Output side: score's video upload path.
  Video::ImageFormat m_format{};
  std::unique_ptr<score::gfx::GPUVideoDecoder> m_decoder;
  std::pair<QShader, QShader> m_shaders;
  QRhiBuffer* m_materialUBO{};
  struct Material
  {
    float scale_w{1.f}, scale_h{1.f};
    float tex_w{}, tex_h{};
  };
  // Frames handed to the decoder stay referenced until the upload that
  // wraps their bytes has been committed: a 3-slot ring covers that.
  AVFrame* m_frames[3]{};
  int m_frameSlot{};
  AVFrame* m_inputFrame{}; ///< Scratch frame the readbacks are copied into.
  std::vector<ossia::audio_vector> m_audio;
  int m_sampleRate{48000};
  int64_t m_audioPos{};
};
}
