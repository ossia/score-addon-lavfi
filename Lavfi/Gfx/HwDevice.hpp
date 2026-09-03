#pragma once

/**
 * @file HwDevice.hpp
 * @brief FFmpeg hardware device contexts for a QRhi.
 *
 * What a filter graph gets as hw_device_ctx, per backend:
 *
 *  - Vulkan backend: an AVVulkanDeviceContext over QRhi's own VkInstance /
 *    VkPhysicalDevice / VkDevice, so FFmpeg's Vulkan filters run on the same
 *    device score renders with and frames can be shared without a copy
 *    (Lavfi::VulkanTransport). Score creates that device itself with every
 *    feature the GPU reports (Gfx/Graph/VulkanVideoDevice.hpp), which is what
 *    libavfilter's Vulkan code needs (timeline semaphores, synchronization2,
 *    storage images without format...). A QRhi-created fallback device lacks
 *    those, and is refused.
 *    This is the code of DirectVideoNodeRenderer::setupHardwareDecoder in
 *    score-plugin-gfx, which is private to that renderer; copied here (score
 *    is not modified by this addon), plus device_features, which score leaves
 *    unset and libplacebo reads.
 *
 *  - any backend on NVIDIA: a CUDA device, derived from the Vulkan one when
 *    there is one (same GPU, FFmpeg's Vulkan<->CUDA transfer then stays on the
 *    GPU), else created through score's device policy. Lets
 *    `hwupload_cuda,scale_cuda,hwdownload` style graphs run.
 *
 *  - D3D11 backend (Windows): an AVD3D11VADeviceContext over QRhi's device
 *    (`scale_d3d11`, AMF `vpp_amf`). VideoToolbox (macOS): FFmpeg's own device
 *    (`scale_vt`, `yadif_videotoolbox`). Both feed the CPU transport through
 *    hwupload/hwdownload in the graph; direct frame sharing is a later phase.
 *
 * Contexts are cached per native device and refcounted through AVBufferRef,
 * so every lavfi node of a QRhi shares one AVHWDeviceContext.
 */

#include <QtGui/private/qrhi_p.h>

#include <vector>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
}

namespace Lavfi
{
/// New reference to the Vulkan device context of @p rhi, or nullptr when the
/// backend is not Vulkan or the device is not score's shared one.
AVBufferRef* vulkanDeviceForRhi(QRhi& rhi);

/// New reference to a CUDA device context usable with @p rhi (derived from the
/// Vulkan device when possible), or nullptr when CUDA is unavailable.
AVBufferRef* cudaDeviceForRhi(QRhi& rhi);

/// New reference to the D3D11VA device context over QRhi's ID3D11Device
/// (Windows, D3D11 backend), else nullptr.
AVBufferRef* d3d11DeviceForRhi(QRhi& rhi);

/// New reference to the best hardware device for filters on this backend:
/// Vulkan, else D3D11, else VideoToolbox (macOS), else CUDA, else nullptr.
/// @p type receives the device type of the returned context.
AVBufferRef* preferredDeviceForRhi(QRhi& rhi, AVHWDeviceType* type = nullptr);

/**
 * @brief The device a given graph needs, or the preferred one.
 *
 * A graph naming `*_vulkan` filters or `libplacebo` can only run on a Vulkan
 * device, and one naming `*_cuda` only on a CUDA device: handing it whatever
 * the render backend prefers makes libavfilter try to insert a conversion
 * between two hardware formats and refuse the graph. Falls back to
 * preferredDeviceForRhi when the graph says nothing.
 */
AVBufferRef* deviceForScript(QRhi& rhi, const std::string& script, AVHWDeviceType* type = nullptr);

/// Human-readable device type for logs.
const char* deviceTypeName(AVHWDeviceType t) noexcept;

/// Extra Vulkan device extensions to report to FFmpeg as enabled, on top of
/// score's sharedVulkanDeviceExtensions(). Only for a caller that created the
/// VkDevice itself with those extensions (the GPU test does, to give FFmpeg
/// 6.1 the VK_EXT_descriptor_buffer it insists on); reporting an extension the
/// device was not created with makes FFmpeg resolve null entry points.
void addVulkanDeviceExtensions(std::vector<const char*> extensions);
}
