#include "HwDevice.hpp"

#include <Lavfi/Core/Graph.hpp>

#include <Video/GpuFormats.hpp>

#include <QDebug>

#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <score/gfx/Vulkan.hpp>

#if QT_HAS_VULKAN
#include <Gfx/Graph/VulkanVideoDevice.hpp>
#include <Gfx/Graph/interop/VkExternalMemoryHelpers.hpp>

#include <QtGui/private/qrhivulkan_p.h>
#include <qvulkanfunctions.h>
#include <vulkan/vulkan.h>
#if __has_include(<libavutil/hwcontext_vulkan.h>)
extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}
#define LAVFI_HAS_VULKAN_HWCONTEXT 1
#endif
#if defined(__linux__)
#include <dlfcn.h>
#elif defined(_WIN32)
#include <windows.h>
#endif
#endif

#if defined(_WIN32) && __has_include(<libavutil/hwcontext_d3d11va.h>)
#include <QtGui/private/qrhid3d11_p.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#define LAVFI_HAS_D3D11_HWCONTEXT 1
#endif

namespace Lavfi
{
namespace
{
// One AVHWDeviceContext per native device. The map holds its own reference so
// the context outlives any individual node; callers get their own ref.
struct DeviceCache
{
  std::mutex mutex;
  std::map<const void*, AVBufferRef*> vulkan;
  std::map<const void*, AVBufferRef*> cuda;
  std::map<const void*, AVBufferRef*> d3d11;
  AVBufferRef* videotoolbox{};
  AVBufferRef* cudaStandalone{};

  static DeviceCache& instance()
  {
    static DeviceCache c;
    return c;
  }
};

AVBufferRef* cached(std::map<const void*, AVBufferRef*>& m, const void* key)
{
  if(auto it = m.find(key); it != m.end())
    return av_buffer_ref(it->second);
  return nullptr;
}
}

static std::vector<const char*>& extraVulkanExtensions()
{
  static std::vector<const char*> v;
  return v;
}

void addVulkanDeviceExtensions(std::vector<const char*> extensions)
{
  auto& v = extraVulkanExtensions();
  v.insert(v.end(), extensions.begin(), extensions.end());
}

const char* deviceTypeName(AVHWDeviceType t) noexcept
{
  return t == AV_HWDEVICE_TYPE_NONE ? "none" : av_hwdevice_get_type_name(t);
}

// ---------------------------------------------------------------------------
//  Vulkan
// ---------------------------------------------------------------------------

AVBufferRef* vulkanDeviceForRhi(QRhi& rhi)
{
#if QT_HAS_VULKAN && defined(LAVFI_HAS_VULKAN_HWCONTEXT) \
    && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  if(rhi.backend() != QRhi::Vulkan)
    return nullptr;
  auto* nh = static_cast<const QRhiVulkanNativeHandles*>(rhi.nativeHandles());
  if(!nh || !nh->dev || !nh->physDev || !nh->inst)
    return nullptr;
  // libavfilter's Vulkan code requires timeline semaphores, synchronization2
  // and friends to have been enabled at vkCreateDevice. score's own shared
  // device enables everything the GPU reports; a QRhi-created device does not,
  // and av_hwdevice_ctx_init would not notice until the first filter runs.
  if(!score::gfx::vkinterop::deviceTimelineSemaphoresEnabled())
    return nullptr;

  auto& cache = DeviceCache::instance();
  std::lock_guard lock{cache.mutex};
  if(auto ref = cached(cache.vulkan, nh->dev))
    return ref;

  AVBufferRef* hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
  if(!hw)
    return nullptr;
  auto* devCtx = reinterpret_cast<AVHWDeviceContext*>(hw->data);
  auto* vk = static_cast<AVVulkanDeviceContext*>(devCtx->hwctx);

  vk->inst = nh->inst->vkInstance();
  vk->phys_dev = nh->physDev;
  vk->act_dev = nh->dev;

  // The system loader's vkGetInstanceProcAddr: Qt's dispatch table does not
  // carry every extension entry point FFmpeg resolves.
#if defined(__linux__)
  {
    static void* libvk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
    if(!libvk)
      libvk = dlopen("libvulkan.so.1", RTLD_NOW);
    if(libvk)
      vk->get_proc_addr
          = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libvk, "vkGetInstanceProcAddr"));
  }
#elif defined(_WIN32)
  {
    static HMODULE libvk = GetModuleHandleA("vulkan-1.dll");
    if(!libvk)
      libvk = LoadLibraryA("vulkan-1.dll");
    if(libvk)
      vk->get_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
          GetProcAddress(libvk, "vkGetInstanceProcAddr"));
  }
#else
  vk->get_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      nh->inst->getInstanceProcAddr("vkGetInstanceProcAddr"));
#endif
  if(!vk->get_proc_addr)
  {
    av_buffer_unref(&hw);
    return nullptr;
  }

  // What FFmpeg keeps pointers to (the extension list, the feature chain)
  // lives as long as the context itself: allocated here, freed by the
  // context's free callback, one set per device.
  struct Storage
  {
    std::vector<const char*> extensions;
    VkPhysicalDeviceVulkan11Features f11{};
    VkPhysicalDeviceVulkan12Features f12{};
    VkPhysicalDeviceVulkan13Features f13{};
  };
  auto* storage = new Storage;
  devCtx->user_opaque = storage;
  devCtx->free = [](AVHWDeviceContext* c) { delete static_cast<Storage*>(c->user_opaque); };

  // Extensions: what score enabled on the shared device, filtered against
  // what the physical device has. Reporting an unavailable one makes FFmpeg
  // resolve null entry points. The string literals live for the process.
  auto* funcs = nh->inst->functions();
  uint32_t extCount = 0;
  funcs->vkEnumerateDeviceExtensionProperties(nh->physDev, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> exts(extCount);
  funcs->vkEnumerateDeviceExtensionProperties(nh->physDev, nullptr, &extCount, exts.data());
  {
    auto wanted = score::gfx::sharedVulkanDeviceExtensions();
    for(auto* ext : extraVulkanExtensions())
      wanted.push_back(ext);
    for(auto* ext : wanted)
      for(auto& e : exts)
        if(std::strcmp(e.extensionName, ext) == 0)
        {
          storage->extensions.push_back(ext);
          break;
        }
  }
  vk->enabled_dev_extensions = storage->extensions.data();
  vk->nb_enabled_dev_extensions = int(storage->extensions.size());

  // Features: the shared device was created with every feature the GPU
  // reports (minus the robustness ones); say so. libplacebo reads this chain.
  auto& f11 = storage->f11;
  auto& f12 = storage->f12;
  auto& f13 = storage->f13;
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  f12.pNext = &f13;
  f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  f11.pNext = &f12;
  vk->device_features = {};
  vk->device_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  vk->device_features.pNext = &f11;
  if(auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
         nh->inst->getInstanceProcAddr("vkGetPhysicalDeviceFeatures2")))
  {
    getFeatures2(nh->physDev, &vk->device_features);
    vk->device_features.features.robustBufferAccess = VK_FALSE;
    f13.robustImageAccess = VK_FALSE;
  }

  // Queue families.
  uint32_t qfCount = 0;
  funcs->vkGetPhysicalDeviceQueueFamilyProperties(nh->physDev, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qfProps(qfCount);
  funcs->vkGetPhysicalDeviceQueueFamilyProperties(nh->physDev, &qfCount, qfProps.data());
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(59, 39, 100)
  int nb_qf = 0;
  for(uint32_t i = 0; i < qfCount && nb_qf < 64; i++)
  {
    vk->qf[nb_qf].idx = int(i);
    vk->qf[nb_qf].num = 1; // score's shared device creates one queue per family
    vk->qf[nb_qf].flags = static_cast<VkQueueFlagBits>(qfProps[i].queueFlags);
    nb_qf++;
  }
  vk->nb_qf = nb_qf;
#else
  const auto firstFamilyWith = [&](VkQueueFlags bits) -> int {
    for(uint32_t i = 0; i < qfCount; i++)
      if((qfProps[i].queueFlags & bits) == bits)
        return int(i);
    return -1;
  };
  const int gfxFam = firstFamilyWith(VK_QUEUE_GRAPHICS_BIT);
  const int txFam = firstFamilyWith(VK_QUEUE_TRANSFER_BIT);
  const int compFam = firstFamilyWith(VK_QUEUE_COMPUTE_BIT);
  vk->queue_family_index = gfxFam;
  vk->nb_graphics_queues = gfxFam >= 0 ? 1 : 0;
  vk->queue_family_tx_index = txFam >= 0 ? txFam : gfxFam;
  vk->nb_tx_queues = 1;
  vk->queue_family_comp_index = compFam >= 0 ? compFam : gfxFam;
  vk->nb_comp_queues = 1;
#ifdef VK_QUEUE_VIDEO_DECODE_BIT_KHR
  const int decFam = firstFamilyWith(VK_QUEUE_VIDEO_DECODE_BIT_KHR);
#else
  const int decFam = -1;
#endif
  vk->queue_family_decode_index = decFam;
  vk->nb_decode_queues = decFam >= 0 ? 1 : 0;
#ifdef VK_QUEUE_VIDEO_ENCODE_BIT_KHR
  const int encFam = firstFamilyWith(VK_QUEUE_VIDEO_ENCODE_BIT_KHR);
#else
  const int encFam = -1;
#endif
  vk->queue_family_encode_index = encFam;
  vk->nb_encode_queues = encFam >= 0 ? 1 : 0;
#endif

  const int ret = av_hwdevice_ctx_init(hw);
  if(ret < 0)
  {
    qDebug() << "lavfi: shared Vulkan device context init failed:" << ret;
    av_buffer_unref(&hw);
    return nullptr;
  }
  cache.vulkan[nh->dev] = hw;
  return av_buffer_ref(hw);
#else
  return nullptr;
#endif
}

// ---------------------------------------------------------------------------
//  CUDA
// ---------------------------------------------------------------------------

AVBufferRef* cudaDeviceForRhi(QRhi& rhi)
{
  if(av_hwdevice_find_type_by_name("cuda") == AV_HWDEVICE_TYPE_NONE)
    return nullptr;
  auto& cache = DeviceCache::instance();

  // Derived from the Vulkan device when there is one: FFmpeg matches the
  // physical device by UUID, so Vulkan<->CUDA transfers stay on that GPU.
  if(AVBufferRef* vk = vulkanDeviceForRhi(rhi))
  {
    std::lock_guard lock{cache.mutex};
    if(auto ref = cached(cache.cuda, vk->data))
    {
      av_buffer_unref(&vk);
      return ref;
    }
    AVBufferRef* cuda = nullptr;
    const int ret = av_hwdevice_ctx_create_derived(&cuda, AV_HWDEVICE_TYPE_CUDA, vk, 0);
    if(ret >= 0 && cuda)
    {
      cache.cuda[vk->data] = cuda;
      av_buffer_unref(&vk);
      return av_buffer_ref(cuda);
    }
    qDebug() << "lavfi: could not derive a CUDA device from Vulkan:" << ret;
    av_buffer_unref(&vk);
  }

  std::lock_guard lock{cache.mutex};
  if(cache.cudaStandalone)
    return av_buffer_ref(cache.cudaStandalone);
  AVBufferRef* cuda = nullptr;
#if SCORE_HAS_LIBAV && LIBAVUTIL_VERSION_MAJOR >= 57
  if(::Video::createHardwareDevice(&cuda, AV_HWDEVICE_TYPE_CUDA) != 0 || !cuda)
    return nullptr;
#else
  if(av_hwdevice_ctx_create(&cuda, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0 || !cuda)
    return nullptr;
#endif
  cache.cudaStandalone = cuda;
  return av_buffer_ref(cuda);
}

// ---------------------------------------------------------------------------
//  D3D11
// ---------------------------------------------------------------------------

AVBufferRef* d3d11DeviceForRhi(QRhi& rhi)
{
#if defined(LAVFI_HAS_D3D11_HWCONTEXT)
  if(rhi.backend() != QRhi::D3D11)
    return nullptr;
  auto* nh = static_cast<const QRhiD3D11NativeHandles*>(rhi.nativeHandles());
  if(!nh || !nh->dev)
    return nullptr;
  auto& cache = DeviceCache::instance();
  std::lock_guard lock{cache.mutex};
  if(auto ref = cached(cache.d3d11, nh->dev))
    return ref;

  AVBufferRef* hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if(!hw)
    return nullptr;
  auto* devCtx = reinterpret_cast<AVHWDeviceContext*>(hw->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(devCtx->hwctx);
  d3d->device = static_cast<ID3D11Device*>(nh->dev);
  d3d->device->AddRef(); // the context releases it on free
  const int ret = av_hwdevice_ctx_init(hw);
  if(ret < 0)
  {
    qDebug() << "lavfi: shared D3D11 device context init failed:" << ret;
    av_buffer_unref(&hw);
    return nullptr;
  }
  cache.d3d11[nh->dev] = hw;
  return av_buffer_ref(hw);
#else
  (void)rhi;
  return nullptr;
#endif
}

// ---------------------------------------------------------------------------

AVBufferRef* preferredDeviceForRhi(QRhi& rhi, AVHWDeviceType* type)
{
  if(type)
    *type = AV_HWDEVICE_TYPE_NONE;
  if(auto vk = vulkanDeviceForRhi(rhi))
  {
    if(type)
      *type = AV_HWDEVICE_TYPE_VULKAN;
    return vk;
  }
  if(auto d3d = d3d11DeviceForRhi(rhi))
  {
    if(type)
      *type = AV_HWDEVICE_TYPE_D3D11VA;
    return d3d;
  }
#if defined(__APPLE__)
  {
    auto& cache = DeviceCache::instance();
    std::lock_guard lock{cache.mutex};
    if(!cache.videotoolbox)
      av_hwdevice_ctx_create(
          &cache.videotoolbox, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
    if(cache.videotoolbox)
    {
      if(type)
        *type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
      return av_buffer_ref(cache.videotoolbox);
    }
  }
#endif
  if(auto cuda = cudaDeviceForRhi(rhi))
  {
    if(type)
      *type = AV_HWDEVICE_TYPE_CUDA;
    return cuda;
  }
  return nullptr;
}

/// Can a Vulkan filter actually run on this device? Asking the device is not
/// enough: what FFmpeg needs from it (VK_EXT_descriptor_buffer, up to 7.0)
/// only shows when a filter configures. One answer per device, kept.
bool vulkanDeviceRunsFilters(AVBufferRef* dev)
{
  if(!dev || !hasFilter("hflip_vulkan"))
    return false;

  static std::mutex mutex;
  static std::vector<std::pair<const AVBufferRef*, bool>> known;
  std::lock_guard lock{mutex};
  for(const auto& [d, ok] : known)
    if(d == dev)
      return ok;

  Lavfi::Graph g;
  std::vector<Lavfi::InputConfig> in(1);
  in[0].type = AVMEDIA_TYPE_VIDEO;
  in[0].video.width = 32;
  in[0].video.height = 32;
  in[0].video.format = AV_PIX_FMT_RGBA;
  in[0].video.time_base = {1, 1000000};
  in[0].video.frame_rate = {60, 1};
  Lavfi::SinkConfig sinks;
  sinks.pix_fmts = {AV_PIX_FMT_RGBA};
  sinks.threads = 1;
  std::string err;
  const bool ok = g.init("hwupload,hflip_vulkan,hwdownload,format=rgba", in, sinks, dev, err);
  if(!ok && qEnvironmentVariableIsSet("SCORE_LAVFI_DEBUG"))
    qDebug() << "lavfi: score's Vulkan device cannot run the filters:"
             << QString::fromStdString(err);
  known.emplace_back(dev, ok);
  return ok;
}

AVBufferRef* deviceForScript(QRhi& rhi, const std::string& script, AVHWDeviceType* type)
{
  const auto mentions = [&](std::string_view what) {
    return script.find(what) != std::string::npos;
  };

  if(mentions("_cuda"))
  {
    if(auto cuda = cudaDeviceForRhi(rhi))
    {
      if(type)
        *type = AV_HWDEVICE_TYPE_CUDA;
      return cuda;
    }
  }
  else if(mentions("_vulkan") || mentions("libplacebo"))
  {
    if(auto vk = vulkanDeviceForRhi(rhi); vk && vulkanDeviceRunsFilters(vk))
    {
      if(type)
        *type = AV_HWDEVICE_TYPE_VULKAN;
      return vk;
    }
    else if(vk)
    {
      av_buffer_unref(&vk);
    }
    // Either score is not rendering with Vulkan, or the device it renders
    // with cannot run the filters (FFmpeg 6.1's need VK_EXT_descriptor_buffer,
    // which score's shared device does not enable). A device of our own runs
    // them, at the cost of the copies this graph's own hwupload does anyway --
    // without it the node has nothing to show at all.
    AVBufferRef* own{};
    if(av_hwdevice_ctx_create(&own, AV_HWDEVICE_TYPE_VULKAN, nullptr, nullptr, 0) >= 0)
    {
      if(type)
        *type = AV_HWDEVICE_TYPE_VULKAN;
      return own;
    }
  }
  return preferredDeviceForRhi(rhi, type);
}
}
