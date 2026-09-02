#include "VulkanTransport.hpp"

#include <Lavfi/Gfx/HwDevice.hpp>

#include <QDebug>

#include <score/gfx/Vulkan.hpp>

#if QT_HAS_VULKAN && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0) \
    && __has_include(<libavutil/hwcontext_vulkan.h>)
#define LAVFI_VULKAN_TRANSPORT 1
#include <Gfx/Graph/interop/VkExternalMemoryHelpers.hpp>

#include <QtGui/private/qrhivulkan_p.h>
#include <qvulkanfunctions.h>
#include <vulkan/vulkan.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}
#endif

#include <map>
#include <type_traits>

namespace Lavfi
{
#if defined(LAVFI_VULKAN_TRANSPORT)

static bool lavfiVkDebug()
{
  static const bool on = qEnvironmentVariableIsSet("SCORE_LAVFI_DEBUG");
  return on;
}

struct VulkanTransport::Impl
{
  QRhi* rhi{};
  VkDevice dev{};
  VkQueue queue{};
  uint32_t queueFamily{};
  QVulkanDeviceFunctions* df{};
  PFN_vkQueueSubmit vkQueueSubmit{};
  int samples{1};

  struct Wrapped
  {
    QRhiTexture* texture{};
    score::gfx::TextureRenderTarget rt;
  };

  struct Input
  {
    QSize size;
    AVBufferRef* frames{};
    AVFrame* current{};  ///< render target of this render frame (not yet submitted)
    AVFrame* rendered{}; ///< render target of the previous frame (submitted)
    std::map<VkImage, Wrapped> wrapped;
  };
  std::vector<Input> inputs;

  // Output ring: the frame being sampled and the previous ones, released two
  // frames later once QRhi's sampling submission is certainly behind us.
  // `layout` is what QRhi's tracking said about the image when the texture
  // object moved on to the next frame: that is the state FFmpeg gets back.
  struct Slot
  {
    AVFrame* frame{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  };
  static constexpr int RingSize = 3;
  Slot ring[RingSize]{};
  int ringSlot{};
  int lastSlot{-1};

  static AVVkFrame* vkframe(AVFrame* f)
  {
    return f ? reinterpret_cast<AVVkFrame*>(f->data[0]) : nullptr;
  }
  static AVHWFramesContext* hwFramesCtx(AVFrame* f)
  {
    return f && f->hw_frames_ctx ? reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data)
                                 : nullptr;
  }
  static AVVulkanFramesContext* framesCtx(AVFrame* f)
  {
    auto* fc = hwFramesCtx(f);
    return fc ? static_cast<AVVulkanFramesContext*>(fc->hwctx) : nullptr;
  }

  // An empty submission that only waits on / signals a timeline value.
  // Submission order on the queue gives the ordering against QRhi's own
  // command buffers without touching them.
  bool submitTimeline(VkSemaphore sem, uint64_t value, bool signal)
  {
    VkTimelineSemaphoreSubmitInfo tl{};
    tl.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext = &tl;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if(signal)
    {
      tl.signalSemaphoreValueCount = 1;
      tl.pSignalSemaphoreValues = &value;
      si.signalSemaphoreCount = 1;
      si.pSignalSemaphores = &sem;
    }
    else
    {
      tl.waitSemaphoreValueCount = 1;
      tl.pWaitSemaphoreValues = &value;
      si.waitSemaphoreCount = 1;
      si.pWaitSemaphores = &sem;
      si.pWaitDstStageMask = &stage;
    }
    return vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
  }

  /// Wait (on the queue) for FFmpeg's last use of the frame.
  bool waitFrame(AVFrame* f)
  {
    auto* vkf = vkframe(f);
    auto* fc = hwFramesCtx(f);
    auto* vkfc = framesCtx(f);
    if(!vkf || !vkfc || vkf->sem[0] == VK_NULL_HANDLE)
      return false;
    vkfc->lock_frame(fc, vkf);
    const VkSemaphore sem = vkf->sem[0];
    const uint64_t value = vkf->sem_value[0];
    vkfc->unlock_frame(fc, vkf);
    return submitTimeline(sem, value, false);
  }

  static uint32_t accessForLayout(VkImageLayout layout)
  {
    switch(layout)
    {
      case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        return VK_ACCESS_TRANSFER_WRITE_BIT;
      case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        return VK_ACCESS_TRANSFER_READ_BIT;
      case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        return VK_ACCESS_SHADER_READ_BIT;
      default:
        return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    }
  }

  /// Record what QRhi left the image in and signal the next timeline value.
  bool releaseFrame(AVFrame* f, VkImageLayout layout)
  {
    auto* vkf = vkframe(f);
    auto* fc = hwFramesCtx(f);
    auto* vkfc = framesCtx(f);
    if(!vkf || !vkfc || vkf->sem[0] == VK_NULL_HANDLE)
      return false;
    vkfc->lock_frame(fc, vkf);
    vkf->layout[0] = layout;
    vkf->access[0]
        = static_cast<std::remove_reference_t<decltype(vkf->access[0])>>(accessForLayout(layout));
    const VkSemaphore sem = vkf->sem[0];
    const uint64_t value = ++vkf->sem_value[0];
    vkfc->unlock_frame(fc, vkf);
    return submitTimeline(sem, value, true);
  }

  static bool singlePlaneRgba(AVPixelFormat sw)
  {
    return sw == AV_PIX_FMT_RGBA || sw == AV_PIX_FMT_BGRA || sw == AV_PIX_FMT_RGB0
           || sw == AV_PIX_FMT_BGR0;
  }
  static QRhiTexture::Format textureFormat(AVPixelFormat sw)
  {
    return (sw == AV_PIX_FMT_BGRA || sw == AV_PIX_FMT_BGR0) ? QRhiTexture::BGRA8
                                                            : QRhiTexture::RGBA8;
  }
};

VulkanTransport::VulkanTransport()
    : m{std::make_unique<Impl>()}
{
}

VulkanTransport::~VulkanTransport()
{
  // release() must have run on the render thread; here only the device ref.
  av_buffer_unref(&m_device);
}

bool VulkanTransport::available(QRhi& rhi)
{
  if(rhi.backend() != QRhi::Vulkan)
    return false;
  auto* nh = static_cast<const QRhiVulkanNativeHandles*>(rhi.nativeHandles());
  return nh && nh->dev && nh->inst && nh->gfxQueue
         && score::gfx::vkinterop::deviceTimelineSemaphoresEnabled();
}

bool VulkanTransport::init(
    const score::gfx::RenderState& state, AVBufferRef* device, int samples)
{
  auto& rhi = *state.rhi;
  if(!available(rhi) || !device)
    return false;
  auto* nh = static_cast<const QRhiVulkanNativeHandles*>(rhi.nativeHandles());
  m->rhi = &rhi;
  m->dev = nh->dev;
  m->queue = nh->gfxQueue;
  m->queueFamily = nh->gfxQueueFamilyIdx;
  m->df = nh->inst->deviceFunctions(nh->dev);
  m->samples = samples > 0 ? samples : 1;
  // Device-level entry point through vkGetDeviceProcAddr: the instance-level
  // trampoline crashes on the NVIDIA Windows driver (see HWVulkanShared.hpp).
  if(auto getDevProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
         nh->inst->getInstanceProcAddr("vkGetDeviceProcAddr")))
    m->vkQueueSubmit
        = reinterpret_cast<PFN_vkQueueSubmit>(getDevProc(nh->dev, "vkQueueSubmit"));
  if(!m->vkQueueSubmit)
    return false;
  av_buffer_unref(&m_device);
  m_device = av_buffer_ref(device);
  return m_device != nullptr;
}

int VulkanTransport::addInput(QSize size)
{
  if(!m_device || size.isEmpty())
    return -1;
  AVBufferRef* frames = av_hwframe_ctx_alloc(m_device);
  if(!frames)
    return -1;
  auto* fc = reinterpret_cast<AVHWFramesContext*>(frames->data);
  auto* vkfc = static_cast<AVVulkanFramesContext*>(fc->hwctx);
  fc->format = AV_PIX_FMT_VULKAN;
  fc->sw_format = AV_PIX_FMT_RGBA;
  fc->width = size.width();
  fc->height = size.height();
  // Four in flight: current, rendered, the one the filter reads, one spare
  // while the previous returns to the pool. The pool grows if a filter keeps
  // more.
  fc->initial_pool_size = 4;
  // The full set: FFmpeg >= 7 ORs SAMPLED|STORAGE|TRANSFER_* into whatever is
  // set here, 6.1 takes the field verbatim. COLOR_ATTACHMENT is what lets a
  // QRhi render pass target the image; SAMPLED is what lets a pass sample the
  // sink image when the filter reuses this pool for its output.
  vkfc->usage = static_cast<VkImageUsageFlagBits>(
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
      | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  const int ret = av_hwframe_ctx_init(frames);
  if(ret < 0)
  {
    qDebug() << "lavfi: Vulkan input frames context failed:" << ret;
    av_buffer_unref(&frames);
    return -1;
  }
  Impl::Input in;
  in.size = size;
  in.frames = frames;
  m->inputs.push_back(std::move(in));
  return int(m->inputs.size()) - 1;
}

AVBufferRef* VulkanTransport::inputFramesContext(int input) const noexcept
{
  return input >= 0 && input < int(m->inputs.size()) ? m->inputs[input].frames : nullptr;
}

bool VulkanTransport::acquireInput(const score::gfx::RenderState& state, int input)
{
  if(input < 0 || input >= int(m->inputs.size()))
    return false;
  auto& in = m->inputs[input];

  AVFrame* f = av_frame_alloc();
  if(av_hwframe_get_buffer(in.frames, f, 0) < 0)
  {
    av_frame_free(&f);
    return false;
  }
  auto* vkf = Impl::vkframe(f);
  if(!vkf || vkf->img[0] == VK_NULL_HANDLE)
  {
    av_frame_free(&f);
    return false;
  }

  // Wrap once per pool image; the pool recycles them.
  auto& rhi = *state.rhi;
  auto it = in.wrapped.find(vkf->img[0]);
  if(it == in.wrapped.end())
  {
    Impl::Wrapped w;
    w.texture = rhi.newTexture(
        QRhiTexture::RGBA8, in.size, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource);
    w.texture->setName("Lavfi::VulkanTransport::input");
    if(!w.texture->createFrom({quint64(vkf->img[0]), int(vkf->layout[0])}))
    {
      delete w.texture;
      av_frame_free(&f);
      return false;
    }
    w.rt = score::gfx::createRenderTarget(state, w.texture, m->samples, false);
    it = in.wrapped.emplace(vkf->img[0], std::move(w)).first;
  }
  else
  {
    // FFmpeg (or our own hand-over) is the last to have touched the image.
    it->second.texture->setNativeLayout(int(vkf->layout[0]));
  }
  m->waitFrame(f);

  // Rotate. A `rendered` that was never taken (takeInput not called) goes
  // back to the pool with the state QRhi left it in, like a taken one.
  if(in.rendered)
  {
    auto* rvkf = Impl::vkframe(in.rendered);
    auto rit = rvkf ? in.wrapped.find(rvkf->img[0]) : in.wrapped.end();
    m->releaseFrame(
        in.rendered,
        rit != in.wrapped.end() ? VkImageLayout(rit->second.texture->nativeTexture().layout)
                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    av_frame_free(&in.rendered);
  }
  in.rendered = in.current;
  in.current = f;
  if(lavfiVkDebug())
    qDebug(
        "lavfi vk: acquire img=%p ffmpeg layout=%d sem=%llu qrhi layout=%d", (void*)vkf->img[0],
        int(vkf->layout[0]), (unsigned long long)vkf->sem_value[0],
        int(it->second.texture->nativeTexture().layout));
  return true;
}

score::gfx::TextureRenderTarget
VulkanTransport::renderTargetForInput(int input) const noexcept
{
  if(input < 0 || input >= int(m->inputs.size()))
    return {};
  auto& in = m->inputs[input];
  auto* vkf = Impl::vkframe(in.current);
  if(!vkf)
    return {};
  auto it = in.wrapped.find(vkf->img[0]);
  return it != in.wrapped.end() ? it->second.rt : score::gfx::TextureRenderTarget{};
}

AVFrame* VulkanTransport::takeInput(int input)
{
  if(input < 0 || input >= int(m->inputs.size()))
    return nullptr;
  auto& in = m->inputs[input];
  AVFrame* f = in.rendered;
  if(!f)
    return nullptr;
  in.rendered = nullptr;
  auto* vkf = Impl::vkframe(f);
  auto it = in.wrapped.find(vkf->img[0]);
  // QRhi tracks the layout it left the attachment in (COLOR_ATTACHMENT_OPTIMAL
  // after a pass, TRANSFER_DST after an upload); tell FFmpeg, and signal that
  // our writes, submitted at the previous endFrame, are done.
  const VkImageLayout layout
      = it != in.wrapped.end() ? VkImageLayout(it->second.texture->nativeTexture().layout)
                               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  m->releaseFrame(f, layout);
  if(lavfiVkDebug())
    qDebug(
        "lavfi vk: handover img=%p layout=%d sem->%llu", (void*)vkf->img[0], int(layout),
        (unsigned long long)vkf->sem_value[0]);
  return f;
}

QRhiTexture* VulkanTransport::presentOutput(
    const score::gfx::RenderState& state, AVFrame* frame)
{
  if(!frame)
    return nullptr;
  auto* vkf = Impl::vkframe(frame);
  auto* fc = Impl::hwFramesCtx(frame);
  if(!vkf || !fc || vkf->img[0] == VK_NULL_HANDLE || !Impl::singlePlaneRgba(fc->sw_format))
  {
    av_frame_free(&frame);
    return nullptr;
  }
  auto& rhi = *state.rhi;
  const QSize size{frame->width, frame->height};
  const QRhiTexture::Format fmt = Impl::textureFormat(fc->sw_format);

  // The layout QRhi left the previous frame's image in, before the texture
  // object moves on: that is what FFmpeg gets back when that slot is released.
  if(m->lastSlot >= 0 && m_outTexture && m->ring[m->lastSlot].frame)
    m->ring[m->lastSlot].layout = VkImageLayout(m_outTexture->nativeTexture().layout);

  if(!m_outTexture || m_outTexture->pixelSize() != size || m_outTexture->format() != fmt)
  {
    if(m_outTexture)
      m_outTexture->deleteLater();
    m_outTexture = rhi.newTexture(fmt, size, 1, {});
    m_outTexture->setName("Lavfi::VulkanTransport::output");
  }
  m_outSwFormat = fc->sw_format;

  // Order QRhi's upcoming submission after the filter's, then hand the
  // image and its layout to QRhi; its own barrier does the transition.
  // createFrom bumps the texture generation, which is what makes the shader
  // resource bindings pick the new image up.
  m->waitFrame(frame);
  if(lavfiVkDebug())
    qDebug(
        "lavfi vk: present img=%p ffmpeg layout=%d sem=%llu", (void*)vkf->img[0],
        int(vkf->layout[0]), (unsigned long long)vkf->sem_value[0]);
  if(!m_outTexture->createFrom({quint64(vkf->img[0]), int(vkf->layout[0])}))
  {
    av_frame_free(&frame);
    return nullptr;
  }

  // Ring: release the slot from two frames ago back to the filter's pool,
  // after telling FFmpeg what QRhi did to it.
  auto& slot = m->ring[m->ringSlot];
  if(slot.frame)
  {
    m->releaseFrame(slot.frame, slot.layout);
    av_frame_free(&slot.frame);
  }
  slot.frame = frame;
  slot.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  m->lastSlot = m->ringSlot;
  m->ringSlot = (m->ringSlot + 1) % Impl::RingSize;
  return m_outTexture;
}

void VulkanTransport::release()
{
  // Nothing may still be in flight when the pool images are destroyed, and
  // QRhi's deferred destruction of the wrapping views must have run first
  // (finish() executes the deferred releases): the views reference the
  // images FFmpeg frees when the frames contexts go.
  if(m->df && m->queue)
    m->df->vkQueueWaitIdle(m->queue);
  for(auto& in : m->inputs)
    for(auto& [img, w] : in.wrapped)
      w.rt.release(); // deleteLater's the (non-owning) texture too
  if(m_outTexture)
  {
    m_outTexture->deleteLater();
    m_outTexture = nullptr;
  }
  if(m->rhi)
    m->rhi->finish();
  for(auto& s : m->ring)
    av_frame_free(&s.frame);
  m->lastSlot = -1;
  m->ringSlot = 0;
  for(auto& in : m->inputs)
  {
    av_frame_free(&in.current);
    av_frame_free(&in.rendered);
    in.wrapped.clear();
    av_buffer_unref(&in.frames);
  }
  m->inputs.clear();
  m_outSwFormat = AV_PIX_FMT_NONE;
}

#else // no Vulkan

struct VulkanTransport::Impl
{
};
VulkanTransport::VulkanTransport()
    : m{std::make_unique<Impl>()}
{
}
VulkanTransport::~VulkanTransport()
{
  av_buffer_unref(&m_device);
}
bool VulkanTransport::available(QRhi&)
{
  return false;
}
bool VulkanTransport::init(const score::gfx::RenderState&, AVBufferRef*, int)
{
  return false;
}
int VulkanTransport::addInput(QSize)
{
  return -1;
}
AVBufferRef* VulkanTransport::inputFramesContext(int) const noexcept
{
  return nullptr;
}
bool VulkanTransport::acquireInput(const score::gfx::RenderState&, int)
{
  return false;
}
score::gfx::TextureRenderTarget VulkanTransport::renderTargetForInput(int) const noexcept
{
  return {};
}
AVFrame* VulkanTransport::takeInput(int)
{
  return nullptr;
}
QRhiTexture* VulkanTransport::presentOutput(const score::gfx::RenderState&, AVFrame* frame)
{
  av_frame_free(&frame);
  return nullptr;
}
void VulkanTransport::release() { }
#endif
}
