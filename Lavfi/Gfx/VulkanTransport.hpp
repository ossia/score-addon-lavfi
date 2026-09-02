#pragma once

/**
 * @file VulkanTransport.hpp
 * @brief GPU-resident frame transport between QRhi (Vulkan backend) and a
 *        libavfilter graph running on the same VkDevice.
 *
 * Input side: the frames the graph consumes come from an FFmpeg
 * AVHWFramesContext pool created with COLOR_ATTACHMENT usage. Each pool image
 * is wrapped once in a QRhiTexture (createFrom) and a render target, so the
 * upstream score node draws straight into FFmpeg's image. Output side: the
 * frame the sink hands back is wrapped in a sampled QRhiTexture the same way.
 * No hwupload/hwdownload, no staging, no host wait.
 *
 * Ordering rides on Vulkan submission order and the frames' own timeline
 * semaphores (AVVkFrame::sem / sem_value, the protocol ff_vk_exec_add_dep_frame
 * implements on FFmpeg's side):
 *
 *   acquire input  : empty submit on the graphics queue WAITING sem >= value,
 *                    so score's render pass starts after FFmpeg's last read.
 *   hand over input: write back layout/access, empty submit SIGNALLING
 *                    value+1 (after endFrame's submit, by queue order), then
 *                    push. FFmpeg waits for that value before reading.
 *   present output : empty submit WAITING sem >= value, so QRhi's next endFrame
 *                    samples after the filter wrote; createFrom + the layout
 *                    FFmpeg left, QRhi adds its own barrier to SHADER_READ.
 *   release output : write back SHADER_READ_ONLY layout, SIGNAL value+1, unref
 *                    (two frames later, after QRhi's sampling submit).
 *
 * Both libavfilter and QRhi submit from the render thread; that is what makes
 * sharing a VkQueue on single-family devices safe.
 */

#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/Utils.hpp>

#include <QSize>

#include <memory>
#include <vector>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace Lavfi
{
class VulkanTransport
{
public:
  VulkanTransport();
  ~VulkanTransport();
  VulkanTransport(const VulkanTransport&) = delete;
  VulkanTransport& operator=(const VulkanTransport&) = delete;

  /// True when @p rhi is a Vulkan QRhi on score's shared device.
  static bool available(QRhi& rhi);

  /// @param device from Lavfi::vulkanDeviceForRhi (a reference is taken).
  bool init(const score::gfx::RenderState& state, AVBufferRef* device);
  void release();
  AVBufferRef* device() const noexcept { return m_device; }

  // ---- inputs -------------------------------------------------------------
  /// Declare one video input of the given size. Returns its index, -1 on failure.
  int addInput(QSize size);
  AVBufferRef* inputFramesContext(int input) const noexcept;
  /// Take a fresh pool frame for this render frame and make it the current
  /// render target of @p input. Call once per render frame, before the passes.
  bool acquireInput(const score::gfx::RenderState& state, int input);
  /// The render target for the input's current frame (empty before acquire).
  score::gfx::TextureRenderTarget renderTargetForInput(int input) const noexcept;
  /// After QRhi submitted the frame that rendered into the current image:
  /// record its state for FFmpeg, signal its semaphore, and return the AVFrame
  /// (owned by the caller: push it, then av_frame_free). nullptr if none.
  AVFrame* takeInput(int input);

  // ---- output ------------------------------------------------------------
  /// Make @p frame (a Vulkan frame from the sink; ownership taken) the texture
  /// to sample this render frame. Returns the wrapping texture, nullptr on failure.
  QRhiTexture* presentOutput(const score::gfx::RenderState& state, AVFrame* frame);
  QRhiTexture* outputTexture() const noexcept { return m_outTexture; }
  /// Pixel format the output texture was wrapped with (RGBA or BGRA order).
  AVPixelFormat outputSwFormat() const noexcept { return m_outSwFormat; }

private:
  struct Impl;
  std::unique_ptr<Impl> m;
  AVBufferRef* m_device{};
  QRhiTexture* m_outTexture{};
  AVPixelFormat m_outSwFormat{AV_PIX_FMT_NONE};
};
}
