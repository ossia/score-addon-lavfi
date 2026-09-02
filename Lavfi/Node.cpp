#include "Node.hpp"

#include <Gfx/Graph/CommonUBOs.hpp>
#include <Gfx/Graph/Utils.hpp>
#include <Gfx/Graph/decoders/GPUVideoDecoder.hpp>
#include <Gfx/Graph/decoders/GPUVideoDecoderFactory.hpp>
#include <Lavfi/AudioNode.hpp>

#include <score/tools/Debug.hpp>

#include <QDebug>

#include <cstring>

extern "C" {
#include <libavutil/imgutils.h>
}

namespace Lavfi
{
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
    : GenericNodeRenderer{node}
{
}

GfxRenderer::~GfxRenderer() = default;

score::gfx::TextureRenderTarget
GfxRenderer::renderTargetForInput(const score::gfx::Port& p)
{
  for(auto& in : m_inputs)
    if(in.port == &p)
      return in.rt;
  return {};
}

void GfxRenderer::inputAboutToFinish(
    score::gfx::RenderList& renderer, const score::gfx::Port& p,
    QRhiResourceUpdateBatch*& res)
{
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

bool GfxRenderer::buildGraph(score::gfx::RenderList& renderer)
{
  auto& n = lavfiNode();
  const auto& prog = n.program();

  std::vector<Lavfi::InputConfig> inputs;
  std::size_t video = 0;
  for(const auto& pad : prog.desc.inputs)
  {
    Lavfi::InputConfig cfg;
    cfg.type = pad.type;
    if(pad.type == AVMEDIA_TYPE_VIDEO)
    {
      if(video >= m_inputs.size() || !m_inputs[video].rt.texture)
        return false;
      const QSize sz = m_inputs[video].rt.texture->pixelSize();
      cfg.video.width = sz.width();
      cfg.video.height = sz.height();
      cfg.video.format = AV_PIX_FMT_RGBA;
      cfg.video.time_base = {1, 1000000};
      cfg.video.frame_rate = {60, 1};
      video++;
    }
    else
    {
      cfg.audio.sample_rate = m_sampleRate;
      cfg.audio.channels = 2;
    }
    inputs.push_back(cfg);
  }

  Lavfi::SinkConfig sinks;
  sinks.pix_fmts = uploadableFormats;
  sinks.sample_rate = m_sampleRate;
  sinks.threads = 0; // slice threading for the CPU filters

  auto g = std::make_unique<Lavfi::Graph>();
  std::string err;
  if(!g->init(prog.script, inputs, sinks, nullptr, err))
  {
    m_error = err;
    qDebug() << "lavfi: graph configuration failed:" << QString::fromStdString(err);
    return false;
  }
  m_graph = std::move(g);
  m_error.clear();
  m_frameCounter = 0;
  m_audioPos = 0;
  if(qEnvironmentVariableIsSet("SCORE_LAVFI_DEBUG"))
    qDebug() << "lavfi: transport=cpu sink=" << av_get_pix_fmt_name(m_graph->outputPixelFormat(0))
             << m_graph->outputWidth(0) << "x" << m_graph->outputHeight(0);
  return true;
}

bool GfxRenderer::setupDecoder(score::gfx::RenderList& renderer, AVPixelFormat fmt, int w, int h)
{
  if(m_decoder)
  {
    m_decoder->release(renderer);
    m_decoder.reset();
  }
  for(auto& [edge, pass] : m_p)
    pass.release();
  m_p.clear();

  m_format = {};
  m_format.width = w;
  m_format.height = h;
  m_format.pixel_format = fmt;
  m_format.color_range = AVCOL_RANGE_JPEG;
  m_decoder = score::gfx::createGPUVideoDecoder(m_format);
  if(!m_decoder)
  {
    qDebug() << "lavfi: no GPU upload path for" << av_get_pix_fmt_name(fmt);
    return false;
  }
  m_shaders = m_decoder->init(renderer);
  if(!m_shaders.first.isValid() || !m_shaders.second.isValid())
    return false;

  score::gfx::defaultPassesInit(
      m_p, this->node.output[0]->edges, renderer, renderer.defaultQuad(), m_shaders.first,
      m_shaders.second, m_processUBO, m_materialUBO, m_decoder->samplers);
  return true;
}

void GfxRenderer::init(score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res)
{
  auto& rhi = *renderer.state.rhi;
  auto& n = lavfiNode();

  m_meshbufs = renderer.initMeshBuffer(renderer.defaultQuad(), res);
  processUBOInit(renderer);
  m_materialUBO = rhi.newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Material));
  m_materialUBO->setName("Lavfi::GfxRenderer::m_materialUBO");
  m_materialUBO->create();

  m_flipY = rhi.isYUpInFramebuffer();
  m_sampleRate = int(n.standardUBO.sampleRate) > 0 ? int(n.standardUBO.sampleRate) : 48000;

  // One render target per texture input, sized like any other 2D input port.
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
    const auto spec = n.resolveRenderTargetSpecs(int32_t(i), renderer);
    in.rt = score::gfx::createRenderTarget(
        renderer.state, QRhiTexture::RGBA8, spec.size, renderer.samples(), false, false,
        QRhiTexture::UsedAsTransferSource);
    m_inputs.push_back(std::move(in));
  }
  m_inputFrame = av_frame_alloc();

  if(buildGraph(renderer))
    setupDecoder(
        renderer, m_graph->outputPixelFormat(0), m_graph->outputWidth(0),
        m_graph->outputHeight(0));
}

void GfxRenderer::applyControls()
{
  if(!m_graph)
    return;
  auto& prog = lavfiNode().program();
  for(auto& [k, v] : const_cast<GfxNode&>(lavfiNode()).takePendingControls())
  {
    if(k < 0 || k >= int(prog.controls.size()))
      continue;
    const auto& o = prog.controls[k];
    m_graph->sendCommand(o.filter, o.name, commandArgument(v));
  }
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
    if(channels != m_graph->inputConfig(idx).audio.channels)
      continue; // layout changed: rebuilt at the next init
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
      continue; // size changed: the graph is rebuilt in update()

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

void GfxRenderer::update(
    score::gfx::RenderList& renderer, QRhiResourceUpdateBatch& res, score::gfx::Edge* edge)
{
  auto& n = lavfiNode();
  res.updateDynamicBuffer(m_processUBO, 0, sizeof(score::gfx::ProcessUBO), &n.standardUBO);

  // Input size changes (upstream resolution change) rebuild the graph.
  bool sizeChanged = false;
  for(auto& in : m_inputs)
  {
    if(!in.rt.texture || !m_graph)
      continue;
    const auto& cfg = m_graph->inputConfig(in.graphInput);
    const QSize sz = in.rt.texture->pixelSize();
    if(sz.width() != cfg.video.width || sz.height() != cfg.video.height)
      sizeChanged = true;
  }
  if(!m_graph || sizeChanged)
  {
    if(!buildGraph(renderer))
      return;
    setupDecoder(
        renderer, m_graph->outputPixelFormat(0), m_graph->outputWidth(0),
        m_graph->outputHeight(0));
  }
  if(!m_graph || !m_decoder)
    return;

  applyControls();
  pushAudio();
  pushReadbacks(renderer);

  // A source graph (no inputs) produces on demand; an effect graph produces
  // when it has consumed a frame. Either way take at most one frame per tick
  // so the graph's own frame rate cannot run ahead of the renderer.
  if(AVFrame* f = m_graph->pullVideo(0))
  {
    if(f->width != m_format.width || f->height != m_format.height
       || f->format != m_format.pixel_format)
    {
      if(!setupDecoder(renderer, AVPixelFormat(f->format), f->width, f->height))
      {
        av_frame_free(&f);
        return;
      }
    }
    m_decoder->exec(renderer, res, *f);
    if(m_decoder->formatChanged)
    {
      // The decoder replaced its textures: rebuild the bindings.
      for(auto& [e, pass] : m_p)
        pass.release();
      m_p.clear();
      score::gfx::defaultPassesInit(
          m_p, this->node.output[0]->edges, renderer, renderer.defaultQuad(), m_shaders.first,
          m_shaders.second, m_processUBO, m_materialUBO, m_decoder->samplers);
      m_decoder->formatChanged = false;
    }
    auto& slot = m_frames[m_frameSlot];
    av_frame_free(&slot);
    slot = f;
    m_frameSlot = (m_frameSlot + 1) % 3;

    Material mat;
    mat.tex_w = float(m_format.width);
    mat.tex_h = float(m_format.height);
    res.updateDynamicBuffer(m_materialUBO, 0, sizeof(Material), &mat);
  }
}

void GfxRenderer::runRenderPass(
    score::gfx::RenderList& renderer, QRhiCommandBuffer& cb, score::gfx::Edge& edge)
{
  if(!m_decoder || !m_decoder->hasFrame)
    return;
  score::gfx::quadRenderPass(renderer, m_meshbufs, cb, edge, m_p);
}

void GfxRenderer::release(score::gfx::RenderList& r)
{
  if(m_decoder)
  {
    m_decoder->release(r);
    m_decoder.reset();
  }
  for(auto& [edge, pass] : m_p)
    pass.release();
  m_p.clear();
  for(auto& in : m_inputs)
    in.rt.release();
  m_inputs.clear();
  for(auto& f : m_frames)
    av_frame_free(&f);
  av_frame_free(&m_inputFrame);
  m_graph.reset();
  delete m_materialUBO;
  m_materialUBO = nullptr;
  defaultRelease(r);
}
}
