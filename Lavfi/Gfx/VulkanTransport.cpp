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

struct VulkanTransport::Impl
{
  VkDevice dev{};
  VkQueue queue{};
  uint32_t queueFamily{};
  QVulkanDeviceFunctions* df{};
  PFN_vkQueueSubmit vkQueueSubmit{};

  struct Wrapped
  {
    QRhiTexture* texture{};
    score::gfx::TextureRenderTarget rt;
  };

  struct Input
  {
    QSize size;
    AVBufferRef* frames{};
    AVFrame* current{}; ///< acquired for this render frame, not yet handed over
    std::map<VkImage, Wrapped> wrapped;
  };
  std::vector<Input> inputs;

  // Output ring: the frame being sampled and the previous ones, released two
  // frames later once QRhi's sampling submission is certainly behind us.
  static constexpr int RingSize = 3;
  AVFrame* ring[RingSize]{};
  int ringSlot{};

  static AVVkFrame* vkframe(AVFrame* f)
  {
    return f ? reinterpret_cast<AVVkFrame*>(f->data[0]) : nullptr;
  }
  static AVVulkanFramesContext* framesCtx(AVFrame* f)
  {
    if(!f || !f->hw_frames_ctx)
      return nullptr;
    auto* fc = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
    return static_cast<AVVulkanFramesContext*>(fc->hwctx);
  }
  static AVHWFramesContext* hwFramesCtx(AVFrame* f)
  {
    return f && f->hw_frames_ctx ? reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data)
                                 : nullptr;
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

  /// Record what we left the image in and signal the next timeline value.
  bool releaseFrame(AVFrame* f, VkImageLayout layout, uint32_t access)
  {
    auto* vkf = vkframe(f);
    auto* fc = hwFramesCtx(f);
    auto* vkfc = framesCtx(f);
    if(!vkf || !vkfc || vkf->sem[0] == VK_NULL_HANDLE)
      return false;
    vkfc->lock_frame(fc, vkf);
    vkf->layout[0] = layout;
    vkf->access[0] = static_cast<std::remove_reference_t<decltype(vkf->access[0])>>(access);
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

bool VulkanTransport::init(const score::gfx::RenderState& state, AVBufferRef* device)
{
  auto& rhi = *state.rhi;
  if(!available(rhi) || !device)
    return false;
  auto* nh = static_cast<const QRhiVulkanNativeHandles*>(rhi.nativeHandles());
  m->dev = nh->dev;
  m->queue = nh->gfxQueue;
  m->queueFamily = nh->gfxQueueFamilyIdx;
  m->df = nh->inst->deviceFunctions(nh->dev);
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
  // Three in flight: the one score renders into, the one the filter reads,
  // one spare while the previous returns to the pool.
  fc->initial_pool_size = 3;
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
  if(in.current)
    return true; // already acquired for this render frame

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
    w.rt = score::gfx::createRenderTarget(state, w.texture, 1, false);
    it = in.wrapped.emplace(vkf->img[0], std::move(w)).first;
  }
  else
  {
    // FFmpeg (or our own hand-over) is the last to have touched the image.
    it->second.texture->setNativeLayout(int(vkf->layout[0]));
  }
  m->waitFrame(f);
  in.current = f;
  if(qEnvironmentVariableIsSet("SCORE_LAVFI_DEBUG"))
    qDebug("lavfi vk: acquire img=%p ffmpeg layout=%d sem=%llu qrhi layout=%d", (void*)vkf->img[0],
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
  AVFrame* f = in.current;
  if(!f)
    return nullptr;
  in.current = nullptr;
  auto* vkf = Impl::vkframe(f);
  auto it = in.wrapped.find(vkf->img[0]);
  // QRhi tracks the layout it left the attachment in (COLOR_ATTACHMENT_OPTIMAL
  // after a pass); tell FFmpeg, and signal that our writes are done.
  const VkImageLayout layout
      = it != in.wrapped.end() ? VkImageLayout(it->second.texture->nativeTexture().layout)
                               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  m->releaseFrame(f, layout, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
  if(qEnvironmentVariableIsSet("SCORE_LAVFI_DEBUG"))
    qDebug("lavfi vk: handover img=%p layout=%d sem->%llu", (void*)vkf->img[0], int(layout),
           (unsigned long long)vkf->sem_value[0]);
  return f;
}

QRhiTexture* VulkanTransport::presentOutput(const score::gfx::RenderState& state, AVFrame* frame)
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
  if(!m_outTexture || m_outTexture->pixelSize() != size)
  {
    delete m_outTexture;
    m_outTexture = rhi.newTexture(QRhiTexture::RGBA8, size, 1, {});
    m_outTexture->setName("Lavfi::VulkanTransport::output");
  }
  m_outSwFormat = fc->sw_format;

  // Order QRhi's upcoming submission after the filter's, then hand the
  // image and its layout to QRhi; its own barrier does the SHADER_READ
  // transition. createFrom bumps the texture generation, which is what makes
  // the shader resource bindings pick the new image up.
  m->waitFrame(frame);
  if(qEnvironmentVariableIsSet("SCORE_LAVFI_DEBUG"))
    qDebug("lavfi vk: present img=%p ffmpeg layout=%d sem=%llu", (void*)vkf->img[0],
           int(vkf->layout[0]), (unsigned long long)vkf->sem_value[0]);
  if(!m_outTexture->createFrom({quint64(vkf->img[0]), int(vkf->layout[0])}))
  {
    av_frame_free(&frame);
    return nullptr;
  }

  // Ring: release the slot from two frames ago back to the filter's pool,
  // after telling FFmpeg what QRhi did to it.
  AVFrame*& slot = m->ring[m->ringSlot];
  if(slot)
  {
    m->releaseFrame(slot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
    av_frame_free(&slot);
  }
  slot = frame;
  m->ringSlot = (m->ringSlot + 1) % Impl::RingSize;
  return m_outTexture;
}

void VulkanTransport::release()
{
  // Nothing may still be in flight when the pool images are destroyed.
  if(m->df && m->queue)
    m->df->vkQueueWaitIdle(m->queue);
  for(auto& f : m->ring)
    av_frame_free(&f);
  for(auto& in : m->inputs)
  {
    av_frame_free(&in.current);
    for(auto& [img, w] : in.wrapped)
      w.rt.release(); // deleteLater's the (non-owning) texture too
    in.wrapped.clear();
    av_buffer_unref(&in.frames);
  }
  m->inputs.clear();
  delete m_outTexture;
  m_outTexture = nullptr;
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
bool VulkanTransport::init(const score::gfx::RenderState&, AVBufferRef*)
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
