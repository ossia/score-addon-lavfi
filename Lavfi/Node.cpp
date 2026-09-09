#include "Node.hpp"

#include <Gfx/Graph/CommonUBOs.hpp>
#include <Gfx/Graph/Utils.hpp>
#include <Gfx/Graph/decoders/GPUVideoDecoder.hpp>
#include <Gfx/Graph/decoders/GPUVideoDecoderFactory.hpp>
#include <Lavfi/AudioNode.hpp>
#include <Lavfi/Gfx/HwDevice.hpp>
#include <Lavfi/Gfx/VulkanTransport.hpp>

#include <score/tools/Debug.hpp>

#include <ossia/detail/algorithms.hpp>

#include <QDebug>

#include <Lavfi/ParseNumber.hpp>

#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace Lavfi
{
static bool lavfiDebug()
{
  static const bool on = qEnvironmentVariableIsSet("SCORE_LAVFI_DEBUG");
  return on;
}

// ---------------------------------------------------------------------------
//  GfxNode
// ---------------------------------------------------------------------------

GfxNode::GfxNode(Program program)
    : m_program{std::move(program)}
{
  // NodeModel expects material storage; there is none, controls travel as messages.
  m_materialData.reset(nullptr);
  // Inputs mirror the exec node's inlets: pads first, then controls.
  int audioIdx = 0;
  for(const auto& pad : m_program.desc.inputs)
  {
    if(pad.type == AVMEDIA_TYPE_VIDEO)
    {
      input.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Image, {}});
      m_portToControl.push_back(-1);
      m_portToAudio.push_back(-1);
    }
    else if(pad.type == AVMEDIA_TYPE_AUDIO)
    {
      input.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Audio, {}});
      m_portToControl.push_back(-1);
      m_portToAudio.push_back(audioIdx++);
    }
  }
  for(std::size_t k = 0; k < m_program.controls.size(); k++)
  {
    input.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Float, {}});
    m_portToControl.push_back(int(k));
    m_portToAudio.push_back(-1);
  }
  for(const auto& pad : m_program.desc.outputs)
    if(pad.type == AVMEDIA_TYPE_VIDEO)
      output.push_back(new score::gfx::Port{this, {}, score::gfx::Types::Image, {}});
}

GfxNode::~GfxNode() = default;

score::gfx::NodeRenderer* GfxNode::createRenderer(score::gfx::RenderList& r) const noexcept
{
  return new GfxRenderer{*this};
}

int GfxNode::controlIndex(int32_t port) const noexcept
{
  return port >= 0 && port < int32_t(m_portToControl.size()) ? m_portToControl[port] : -1;
}

void GfxNode::process(int32_t port, const ossia::value& v)
{
  const int k = controlIndex(port);
  if(k < 0)
  {
    ProcessNode::process(port, v);
    return;
  }
  std::lock_guard lock{m_mutex};
  // Coalesce: only the last value of a control per tick matters.
  for(auto& [p, val] : m_pendingControls)
    if(p == k)
    {
      val = v;
      return;
    }
  m_pendingControls.emplace_back(k, v);
}

void GfxNode::process(int32_t port, const ossia::audio_vector& v)
{
  const int a = port >= 0 && port < int32_t(m_portToAudio.size()) ? m_portToAudio[port] : -1;
  if(a < 0)
    return;
  std::lock_guard lock{m_mutex};
  // Bounded: with no renderer consuming (node not connected to an output, or
  // a stalled render thread) this would otherwise grow at the sample rate.
  constexpr std::size_t maxPending = 8;
  if(m_pendingAudio.size() >= maxPending)
    m_pendingAudio.erase(m_pendingAudio.begin());
  m_pendingAudio.emplace_back(a, v);
}

std::vector<std::pair<int, ossia::value>> GfxNode::takePendingControls()
{
  std::lock_guard lock{m_mutex};
  return std::move(m_pendingControls);
}

std::vector<std::pair<int, ossia::audio_vector>> GfxNode::takePendingAudio()
{
  std::lock_guard lock{m_mutex};
  return std::move(m_pendingAudio);
}

// ---------------------------------------------------------------------------
//  GfxRenderer
// ---------------------------------------------------------------------------

GfxRenderer::GfxRenderer(const GfxNode& node) noexcept
    : NodeRenderer{node}
{
}

GfxRenderer::~GfxRenderer() = default;

score::gfx::TextureRenderTarget
GfxRenderer::renderTargetForInput(const score::gfx::Port& p)
{
  for(auto& in : m_inputs)
  {
    if(in.port != &p)
      continue;
    if(m_transport == Transport::Vulkan && m_vk)
      return m_vk->renderTargetForInput(in.vkInput);
    return in.rt;
  }
  return {};
}

void GfxRenderer::inputAboutToFinish(
    score::gfx::RenderList& renderer, const score::gfx::Port& p,
    QRhiResourceUpdateBatch*& res)
{
  if(m_transport != Transport::Cpu)
    return;
  for(auto& in : m_inputs)
  {
    if(in.port != &p || !in.rt.texture)
      continue;
    if(!res)
      res = renderer.state.rhi->nextResourceUpdateBatch();
    res->readBackTexture(QRhiReadbackDescription{in.rt.texture}, &in.readback);
  }
}

// Pixel formats score::gfx::createGPUVideoDecoder can upload, in preference
// order: lavfi picks the first the graph's last filter can produce, so an
// RGB graph stays RGB and a YUV graph uploads 1.5 bytes/px instead of 4.
static const std::vector<AVPixelFormat> uploadableFormats{
    AV_PIX_FMT_RGBA,    AV_PIX_FMT_BGRA,      AV_PIX_FMT_RGB0,    AV_PIX_FMT_BGR0,
    AV_PIX_FMT_RGB24,   AV_PIX_FMT_YUV420P,   AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,   AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUVA444P,
    AV_PIX_FMT_GRAY8,   AV_PIX_FMT_YUV420P10, AV_PIX_FMT_YUV444P10, AV_PIX_FMT_RGBA64,
    AV_PIX_FMT_GBRP,    AV_PIX_FMT_GBRAP};

GfxRenderer::Transport GfxRenderer::chooseTransport(score::gfx::RenderList& renderer) const
{
  const QByteArray env = qgetenv("SCORE_LAVFI_TRANSPORT").toLower();
  if(env == "cpu")
    return Transport::Cpu;
  if(m_vulkanRefused)
    return Transport::Cpu;
  if(env == "vulkan" || env.isEmpty() || env == "auto")
    if(VulkanTransport::available(*renderer.state.rhi))
      return Transport::Vulkan;
  return Transport::Cpu;
}

bool GfxRenderer::setupTransport(score::gfx::RenderList& renderer, Transport t)
{
  releaseTransport(renderer);
  auto& rhi = *renderer.state.rhi;

  if(t == Transport::Vulkan)
  {
    AVBufferRef* dev = vulkanDeviceForRhi(rhi);
    if(!dev)
      return false;
    m_vk = std::make_unique<VulkanTransport>();
    const bool ok = m_vk->init(renderer.state, dev, renderer.samples());
    av_buffer_unref(&dev);
    if(!ok)
    {
      m_vk.reset();
      return false;
    }
    for(auto& in : m_inputs)
    {
      in.vkInput = m_vk->addInput(in.size);
      // The first image must be a valid render target before the upstream
      // node builds its pipeline against it (addOutputPass on our inputs).
      if(in.vkInput < 0 || !m_vk->acquireInput(renderer.state, in.vkInput))
      {
        m_vk->release();
        m_vk.reset();
        return false;
      }
    }
    m_vkSampler = rhi.newSampler(
        QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge,
        QRhiSampler::ClampToEdge);
    m_vkSampler->create();
    m_transport = Transport::Vulkan;
  }
  else
  {
    // One render target per texture input, sized like any other 2D input port.
    for(auto& in : m_inputs)
    {
      in.rt = score::gfx::createRenderTarget(
          renderer.state, QRhiTexture::RGBA8, in.size, renderer.samples(), false, false,
          QRhiTexture::UsedAsTransferSource);
      if(!in.rt.texture)
        return false;
    }
    m_inputFrame = av_frame_alloc();
    m_transport = Transport::Cpu;
  }
  return true;
}

void GfxRenderer::releaseTransport(score::gfx::RenderList& renderer)
{
  if(m_vk)
  {
    m_vk->release();
    m_vk.reset();
  }
  if(m_vkSampler)
  {
    m_vkSampler->deleteLater();
    m_vkSampler = nullptr;
  }
  m_vkSamplers.clear();
  for(auto& in : m_inputs)
  {
    in.rt.release();
    in.rt = {};
    in.vkInput = -1;
    in.readback = {};
  }
  av_frame_free(&m_inputFrame);
  m_transport = Transport::None;
}

bool GfxRenderer::buildGraph(score::gfx::RenderList& renderer)
{
  auto& n = lavfiNode();
  const auto& prog = n.program();
  const bool vulkan = m_transport == Transport::Vulkan;
  m_rebuildGraph = false;

  std::vector<Lavfi::InputConfig> inputs;
  std::size_t video = 0, audio = 0;
  for(const auto& pad : prog.desc.inputs)
  {
    Lavfi::InputConfig cfg;
    cfg.type = pad.type;
    if(pad.type == AVMEDIA_TYPE_VIDEO)
    {
      if(video >= m_inputs.size())
        return false;
      auto& in = m_inputs[video++];
      cfg.video.width = in.size.width();
      cfg.video.height = in.size.height();
      cfg.video.time_base = {1, 1000000};
      cfg.video.frame_rate = {60, 1};
      if(vulkan)
      {
        cfg.video.format = AV_PIX_FMT_VULKAN;
        cfg.video.hw_frames_ctx = m_vk->inputFramesContext(in.vkInput);
      }
      else
        cfg.video.format = AV_PIX_FMT_RGBA;
    }
    else
    {
      cfg.audio.sample_rate = prog.sampleRate;
      if(audio >= m_audioChannels.size())
        m_audioChannels.resize(audio + 1, 2);
      cfg.audio.channels = std::max(1, m_audioChannels[audio++]);
    }
    inputs.push_back(cfg);
  }

  Lavfi::SinkConfig sinks;
  sinks.pix_fmts = vulkan ? std::vector<AVPixelFormat>{AV_PIX_FMT_VULKAN} : uploadableFormats;
  sinks.sample_rate = prog.sampleRate;
  sinks.threads = 0; // slice threading for the CPU filters

  auto g = std::make_unique<Lavfi::Graph>();
  std::string err;
  if(!g->init(prog.script, inputs, sinks, m_hwDevice, err, buildTimeOptions()))
  {
    m_error = err;
    if(lavfiDebug() || !vulkan)
      qDebug() << "lavfi: graph configuration failed (" << (vulkan ? "vulkan" : "cpu")
               << "):" << QString::fromStdString(err);
    return false;
  }
  if(vulkan)
  {
    // The sink must give single-plane RGBA-order frames to be sampled as is.
    AVBufferRef* hw = g->outputHwFramesContext(0);
    const auto* fc = hw ? reinterpret_cast<AVHWFramesContext*>(hw->data) : nullptr;
    const AVPixelFormat sw = fc ? fc->sw_format : AV_PIX_FMT_NONE;
    if(g->outputPixelFormat(0) != AV_PIX_FMT_VULKAN
       || !(sw == AV_PIX_FMT_RGBA || sw == AV_PIX_FMT_BGRA || sw == AV_PIX_FMT_RGB0
            || sw == AV_PIX_FMT_BGR0))
    {
      m_error = std::string("Vulkan sink format not samplable: ")
                + (sw != AV_PIX_FMT_NONE ? av_get_pix_fmt_name(sw) : "?");
      if(lavfiDebug())
        qDebug() << "lavfi:" << QString::fromStdString(m_error);
      return false;
    }
  }
  m_graph = std::move(g);
  m_error.clear();
  m_frameCounter = 0;
  m_audioPos = 0;
  m_hasOutput = false;
  if(lavfiDebug())
    qDebug() << "lavfi: transport=" << (vulkan ? "vulkan" : "cpu")
             << "device=" << deviceTypeName(m_hwDeviceType)
             << "sink=" << av_get_pix_fmt_name(m_graph->outputPixelFormat(0))
             << m_graph->outputWidth(0) << "x" << m_graph->outputHeight(0)
             << "filters=" << m_graph->description().filters.size();
  return true;
}

bool GfxRenderer::currentShaders(
    const QShader*& vs, const QShader*& fs, std::span<const score::gfx::Sampler>& samplers) const
{
  if(m_transport == Transport::Vulkan)
  {
    if(!m_vkVertex.isValid() || !m_vkFragment.isValid() || m_vkSamplers.empty())
      return false;
    vs = &m_vkVertex;
    fs = &m_vkFragment;
    samplers = m_vkSamplers;
    return true;
  }
  if(!m_decoder || !m_shaders.first.isValid() || !m_shaders.second.isValid())
    return false;
  vs = &m_shaders.first;
  fs = &m_shaders.second;
  samplers = m_decoder->samplers;
  return true;
}

void GfxRenderer::addOutputPass(
    score::gfx::RenderList& renderer, score::gfx::Edge& edge, QRhiResourceUpdateBatch& res)
{
  const QShader* vs{};
  const QShader* fs{};
  std::span<const score::gfx::Sampler> samplers;
  if(!currentShaders(vs, fs, samplers))
    return;
  // RenderList's surgical re-init adds a pass per edge after initState,
  // which already built them.
  if(hasOutputPassForEdge(edge))
    return;
  auto rt = renderer.renderTargetForOutput(edge);
  if(!rt.renderTarget)
    return;
  auto pip = score::gfx::buildPipeline(
      renderer, renderer.defaultQuad(), *vs, *fs, rt, m_processUBO, m_materialUBO, samplers);
  if(pip.pipeline)
    m_p.emplace_back(&edge, score::gfx::Pass{rt, pip, nullptr});
}

void GfxRenderer::removeOutputPass(score::gfx::RenderList& renderer, score::gfx::Edge& edge)
{
  auto it = ossia::find_if(m_p, [&](const auto& p) { return p.first == &edge; });
  if(it != m_p.end())
  {
    it->second.release();
    m_p.erase(it);
  }
}

bool GfxRenderer::hasOutputPassForEdge(score::gfx::Edge& edge) const
{
  return ossia::find_if(m_p, [&](const auto& p) { return p.first == &edge; }) != m_p.end();
}

void GfxRenderer::rebuildPasses(score::gfx::RenderList& renderer)
{
  for(auto& [edge, pass] : m_p)
    pass.release();
  m_p.clear();
  if(this->node.output.empty())
    return;
  auto* batch = renderer.state.rhi->nextResourceUpdateBatch();
  for(auto* edge : this->node.output[0]->edges)
    addOutputPass(renderer, *edge, *batch);
  // Nothing was queued on it; hand it back.
  batch->release();
}

bool GfxRenderer::setupOutputPass(score::gfx::RenderList& renderer)
{
  // Vulkan transport: a passthrough pass sampling the sink image. The
  // texture object is created by the transport on first present; until then
  // sample the renderer's empty texture so the bindings are complete.
  static const QString frag = QString(R"_(#version 450

)_" SCORE_GFX_VIDEO_UNIFORMS R"_(

layout(binding=3) uniform sampler2D y_tex;
layout(location = 0) in vec2 v_texcoord;
layout(location = 0) out vec4 fragColor;

void main()
{
  fragColor = texture(y_tex, v_texcoord);
}
)_");
  std::tie(m_vkVertex, m_vkFragment) = score::gfx::makeShaders(
      renderer.state, score::gfx::GPUVideoDecoder::vertexShader(), frag);
  if(!m_vkVertex.isValid() || !m_vkFragment.isValid())
    return false;
  m_vkSamplers.clear();
  QRhiTexture* tex = m_vk ? m_vk->outputTexture() : nullptr;
  m_vkSamplers.push_back({m_vkSampler, tex ? tex : &renderer.emptyTexture()});
  rebuildPasses(renderer);
  return true;
}

bool GfxRenderer::setupDecoder(score::gfx::RenderList& renderer, AVPixelFormat fmt, int w, int h)
{
  if(m_decoder)
  {
    m_decoder->release(renderer);
    m_decoder.reset();
  }
  m_format = {};
  m_format.width = w;
  m_format.height = h;
  m_format.pixel_format = fmt;
  m_format.color_range = AVCOL_RANGE_JPEG;
  m_decoder = score::gfx::createGPUVideoDecoder(m_format);
  if(!m_decoder)
  {
    qDebug() << "lavfi: no GPU upload path for" << av_get_pix_fmt_name(fmt);
    rebuildPasses(renderer);
    return false;
  }
  m_shaders = m_decoder->init(renderer);
  if(!m_shaders.first.isValid() || !m_shaders.second.isValid())
    return false;
  rebuildPasses(renderer);
  return true;
}

void GfxRenderer::init(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res)
{
  initState(renderer, res);
  if(!this->node.output.empty())
    for(auto* edge : this->node.output[0]->edges)
      if(!hasOutputPassForEdge(*edge))
        addOutputPass(renderer, *edge, res);
}

void GfxRenderer::release(score::gfx::RenderList& r)
{
  releaseState(r);
}

void GfxRenderer::initState(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res)
{
  if(m_initialized)
    return;
  auto& rhi = *renderer.state.rhi;
  auto& n = lavfiNode();

  m_meshBuffer = renderer.initMeshBuffer(renderer.defaultQuad(), res);
  m_processUBO = rhi.newBuffer(
      QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(score::gfx::ProcessUBO));
  m_processUBO->setName("Lavfi::GfxRenderer::m_processUBO");
  m_processUBO->create();
  m_materialUBO = rhi.newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Material));
  m_materialUBO->setName("Lavfi::GfxRenderer::m_materialUBO");
  m_materialUBO->create();
  m_flipY = rhi.isYUpInFramebuffer();
  m_graphFailed = false;
  m_lastFrame = -1;

  // Inputs: sizes as resolved for any other 2D input port. RenderList
  // re-runs releaseState/initState when a spec changes.
  m_inputs.clear();
  int graphInput = 0;
  for(std::size_t i = 0; i < n.input.size(); i++)
  {
    auto* port = n.input[i];
    if(port->type != score::gfx::Types::Image)
    {
      if(port->type == score::gfx::Types::Audio)
        graphInput++;
      continue;
    }
    Input in;
    in.port = port;
    in.graphInput = graphInput++;
    in.size = n.resolveRenderTargetSpecs(int32_t(i), renderer).size;
    if(in.size.isEmpty())
      in.size = renderer.state.renderSize;
    m_inputs.push_back(std::move(in));
  }

  // Hardware device for the graph's filters, whatever the transport.
  av_buffer_unref(&m_hwDevice);
  // What the graph asks for, not just what the render backend prefers: a
  // graph full of *_vulkan filters cannot run on a CUDA device.
  m_hwDevice = deviceForScript(rhi, n.program().script, &m_hwDeviceType);

  // Transport ladder: Vulkan when possible and accepted by the graph, else CPU.
  Transport t = chooseTransport(renderer);
  if(t == Transport::Vulkan)
  {
    if(!setupTransport(renderer, Transport::Vulkan) || !buildGraph(renderer))
    {
      if(lavfiDebug())
        qDebug() << "lavfi: Vulkan transport refused, falling back to CPU";
      m_vulkanRefused = true;
      m_graph.reset();
      t = Transport::Cpu;
    }
  }
  if(t == Transport::Cpu)
  {
    if(setupTransport(renderer, Transport::Cpu))
      m_graphFailed = !buildGraph(renderer);
  }
  if(m_transport == Transport::Vulkan)
  {
    if(m_hwDeviceType != AV_HWDEVICE_TYPE_VULKAN)
    {
      // The frames live on the Vulkan device: the graph's filters must too.
      av_buffer_unref(&m_hwDevice);
      m_hwDevice = m_vk->device() ? av_buffer_ref(m_vk->device()) : nullptr;
      m_hwDeviceType = AV_HWDEVICE_TYPE_VULKAN;
    }
    setupOutputPass(renderer);
  }
  else if(m_graph)
    setupDecoder(
        renderer, m_graph->outputPixelFormat(0), m_graph->outputWidth(0),
        m_graph->outputHeight(0));
  m_initialized = true;
}

void GfxRenderer::releaseState(score::gfx::RenderList& r)
{
  if(!m_initialized)
    return;
  for(auto& [edge, pass] : m_p)
    pass.release();
  m_p.clear();
  if(m_decoder)
  {
    m_decoder->release(r);
    m_decoder.reset();
  }
  m_shaders = {};
  m_vkVertex = {};
  m_vkFragment = {};
  // The output ring and the pools go with the transport, which waits for the
  // queue and flushes QRhi's deferred releases before the images are freed;
  // the graph (whose pools may back the sink frames) goes after.
  releaseTransport(r);
  m_graph.reset();
  m_inputs.clear();
  for(auto& f : m_frames)
    av_frame_free(&f);
  av_buffer_unref(&m_hwDevice);
  m_hwDeviceType = AV_HWDEVICE_TYPE_NONE;
  delete m_materialUBO;
  m_materialUBO = nullptr;
  delete m_processUBO;
  m_processUBO = nullptr;
  m_meshBuffer = {};
  m_hasOutput = false;
  m_initialized = false;
}

void GfxRenderer::applyControls()
{
  if(!m_graph)
    return;
  auto& prog = lavfiNode().program();
  m_optionValues.resize(prog.controls.size());
  for(auto& [k, v] : const_cast<GfxNode&>(lavfiNode()).takePendingControls())
  {
    if(k < 0 || k >= int(prog.controls.size()))
      continue;
    const auto& o = prog.controls[k];
    const auto arg = commandArgument(v);
    if(!worthSending(o, arg) || m_optionValues[k] == arg)
      continue;
    if(o.runtime)
    {
      m_optionValues[k] = arg;
      m_graph->sendCommand(o.filter, o.name, arg);
    }
    else
    {
      // Read once, when the filter is initialised: the graph has to be built
      // again around the new value. Next update() does it, once, however many
      // of these moved in the meantime.
      m_optionValues[k] = arg;
      m_rebuildGraph = true;
    }
  }
}

std::vector<Lavfi::OptionValue> GfxRenderer::buildTimeOptions() const
{
  const auto& controls = lavfiNode().program().controls;
  std::vector<Lavfi::OptionValue> opts;
  for(std::size_t k = 0; k < controls.size() && k < m_optionValues.size(); k++)
    if(!controls[k].runtime && !m_optionValues[k].empty())
      opts.push_back({controls[k].filter, controls[k].name, m_optionValues[k]});
  return opts;
}

void GfxRenderer::pushAudio()
{
  if(!m_graph)
    return;
  for(auto& [a, samples] : const_cast<GfxNode&>(lavfiNode()).takePendingAudio())
  {
    // Which graph input is the a-th audio pad?
    int idx = -1, seen = 0;
    for(std::size_t i = 0; i < m_graph->description().inputs.size(); i++)
      if(m_graph->description().inputs[i].type == AVMEDIA_TYPE_AUDIO && seen++ == a)
        idx = int(i);
    if(idx < 0 || samples.empty())
      continue;
    const int channels = int(samples.size());
    const int frames = int(samples[0].size());
    if(a >= int(m_audioChannels.size()))
      m_audioChannels.resize(a + 1, 2);
    if(channels != m_graph->inputConfig(idx).audio.channels)
    {
      // Layout changed: the graph is rebuilt at the end of this update.
      m_audioChannels[a] = channels;
      m_rebuildGraph = true;
      continue;
    }
    std::vector<std::vector<float>> planes(channels);
    std::vector<const float*> ptrs(channels);
    for(int c = 0; c < channels; c++)
    {
      planes[c].assign(samples[c].begin(), samples[c].end());
      ptrs[c] = planes[c].data();
    }
    m_graph->pushAudio(idx, ptrs.data(), channels, frames, m_audioPos);
    m_audioPos += frames;
  }
}

void GfxRenderer::pushReadbacks(score::gfx::RenderList& renderer)
{
  for(auto& in : m_inputs)
  {
    auto& rb = in.readback;
    if(rb.data.isEmpty() || rb.pixelSize.isEmpty())
      continue;
    const int w = rb.pixelSize.width(), h = rb.pixelSize.height();
    const auto& cfg = m_graph->inputConfig(in.graphInput);
    if(w != cfg.video.width || h != cfg.video.height)
    {
      // The upstream picture is not the size this graph was built for. One
      // frame of that is a readback from before a re-init and is dropped; if
      // it is the new size, the graph has to be built again around it --
      // otherwise every later frame is dropped too and the node keeps showing
      // the last picture it managed to filter.
      rb.data.clear();
      if(in.size != QSize{w, h})
      {
        in.size = QSize{w, h};
        m_rebuildGraph = true;
      }
      continue;
    }

    AVFrame* f = m_inputFrame;
    av_frame_unref(f);
    f->format = rb.format == QRhiTexture::BGRA8 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
    f->width = w;
    f->height = h;
    if(av_frame_get_buffer(f, 0) < 0)
      continue;
    // Copy (filters may keep references past this call, so the readback's
    // bytes cannot be wrapped), flipping the rows on backends that read back
    // bottom-up so the graph sees top-down frames like everything in FFmpeg.
    const int srcStride = w * 4;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(rb.data.constData());
    if(int(rb.data.size()) < srcStride * h)
      continue;
    for(int y = 0; y < h; y++)
    {
      const int sy = m_flipY ? (h - 1 - y) : y;
      std::memcpy(f->data[0] + y * f->linesize[0], src + sy * srcStride, srcStride);
    }
    f->pts = m_frameCounter;
    m_graph->pushVideo(in.graphInput, f);
    av_frame_unref(f);
    rb.data.clear();
  }
  m_frameCounter++;
}

void GfxRenderer::pushVulkanInputs()
{
  // The image rendered into during the previous render frame was submitted
  // at its endFrame: hand it over. The one rendered into during this frame
  // is handed over next time.
  for(auto& in : m_inputs)
  {
    if(AVFrame* f = m_vk->takeInput(in.vkInput))
    {
      f->pts = m_frameCounter;
      m_graph->pushVideo(in.graphInput, f);
      av_frame_free(&f);
    }
  }
  m_frameCounter++;
}

void GfxRenderer::publishMetadata()
{
  auto& prog = lavfiNode().program();
  if(!prog.metadata)
    return;
  const auto& md = m_graph->lastMetadata(0);
  if(md.empty())
    return;
  // A map, so a receiver addresses a measurement by name (lavfi.r128.M)
  // instead of counting into a list of pairs.
  ossia::value_map_type map;
  map.reserve(md.size());
  for(const auto& [k, v] : md)
  {
    const auto num = Lavfi::parseNumber(v);
    ossia::value val = num ? ossia::value{float(*num)} : ossia::value{v};
    map.emplace_back(k, std::move(val));
  }
  prog.metadata->post(ossia::value{std::move(map)});
}

void GfxRenderer::drainUnusedOutputs()
{
  // Only the first video output is displayed. Anything else the graph
  // produces (a second video pad, ebur128's default video output, the audio
  // of an audio+video graph) would otherwise pile up in its sink forever.
  // Request-free: a source-fed extra output would otherwise produce a frame
  // per request, forever.
  const int n = m_graph->outputCount();
  for(int o = 1; o < n; o++)
    m_graph->discardPending(o);
}

void GfxRenderer::update(
    score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res, score::gfx::Edge* edge)
{
  if(!m_initialized)
    return;
  // Once per render frame, however many edges leave this node.
  if(m_lastFrame == renderer.frame)
    return;
  m_lastFrame = renderer.frame;

  auto& n = lavfiNode();
  res.updateDynamicBuffer(m_processUBO, 0, sizeof(score::gfx::ProcessUBO), &n.standardUBO);

  if(m_graphFailed || m_transport == Transport::None)
    return;
  // Before the build: a control that needs a rebuild has to be in hand when
  // the graph is made, not after.
  applyControls();
  if(!m_graph || m_rebuildGraph)
  {
    if(!buildGraph(renderer))
    {
      m_graphFailed = true;
      return;
    }
    if(m_transport == Transport::Vulkan)
      setupOutputPass(renderer);
    else
      setupDecoder(
          renderer, m_graph->outputPixelFormat(0), m_graph->outputWidth(0),
          m_graph->outputHeight(0));
  }

  pushAudio();

  if(m_transport == Transport::Vulkan)
  {
    pushVulkanInputs();
    // Rotate: this frame's render targets.
    for(auto& in : m_inputs)
      m_vk->acquireInput(renderer.state, in.vkInput);

    if(AVFrame* f = m_graph->pullVideo(0))
    {
      QRhiTexture* prev = m_vk->outputTexture();
      const int w = f->width, h = f->height;
      QRhiTexture* tex = m_vk->presentOutput(renderer.state, f);
      if(!tex)
        return;
      if(tex != prev || !m_hasOutput)
      {
        // First frame, or the texture object was recreated (size change):
        // point the bindings at it. Later frames only bump its generation.
        m_vkSamplers.clear();
        m_vkSamplers.push_back({m_vkSampler, tex});
        rebuildPasses(renderer);
      }
      m_hasOutput = true;
      Material mat;
      mat.tex_w = float(w);
      mat.tex_h = float(h);
      res.updateDynamicBuffer(m_materialUBO, 0, sizeof(Material), &mat);
      publishMetadata();
    }
    drainUnusedOutputs();
    return;
  }

  // --- CPU transport ---------------------------------------------------------
  pushReadbacks(renderer);

  // A source graph (no inputs) produces on demand; an effect graph produces
  // when it has consumed a frame. Either way take at most one frame per tick
  // so the graph's own frame rate cannot run ahead of the renderer.
  if(AVFrame* f = m_graph->pullVideo(0))
  {
    if(!m_decoder || f->width != m_format.width || f->height != m_format.height
       || f->format != m_format.pixel_format)
    {
      if(!setupDecoder(renderer, AVPixelFormat(f->format), f->width, f->height))
      {
        av_frame_free(&f);
        return;
      }
    }
    m_decoder->exec(renderer, res, *f);
    // The software decoders do not raise this themselves: score's own video
    // renderers set it after exec() and gate their render pass on it, and so
    // do we (runRenderPass). Without it the pass never draws and the node
    // shows black however well the graph runs.
    m_decoder->hasFrame = true;
    if(m_decoder->formatChanged)
    {
      rebuildPasses(renderer); // the decoder replaced its textures
      m_decoder->formatChanged = false;
    }
    auto& slot = m_frames[m_frameSlot];
    av_frame_free(&slot);
    slot = f;
    m_frameSlot = (m_frameSlot + 1) % 3;
    m_hasOutput = true;

    Material mat;
    mat.tex_w = float(m_format.width);
    mat.tex_h = float(m_format.height);
    res.updateDynamicBuffer(m_materialUBO, 0, sizeof(Material), &mat);
    publishMetadata();
  }
  drainUnusedOutputs();
}

void GfxRenderer::runRenderPass(
    score::gfx::RenderList& renderer, QRhiCommandBuffer& cb, score::gfx::Edge& edge)
{
  if(!m_hasOutput || m_p.empty())
    return;
  if(m_transport == Transport::Cpu && (!m_decoder || !m_decoder->hasFrame))
    return;
  score::gfx::quadRenderPass(renderer, m_meshBuffer, cb, edge, m_p);
}
}
