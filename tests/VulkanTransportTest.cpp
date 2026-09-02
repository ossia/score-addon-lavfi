// GPU test of the Vulkan transport: score's shared VkDevice, an FFmpeg pool
// image rendered into (well, uploaded to) through QRhi, run through a
// `*_vulkan` filter without leaving the GPU, and the sink image read back
// through QRhi and byte-compared against the expected result.
//
// Needs a Vulkan device and a display; prints SKIP and exits 0 without one.
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/Utils.hpp>
#include <Gfx/Graph/VulkanVideoDevice.hpp>
#include <Gfx/Graph/interop/VkExternalMemoryHelpers.hpp>

#include <score/gfx/Vulkan.hpp>

#include <QtGui/private/qrhivulkan_p.h>
#include <Lavfi/Core/Graph.hpp>
#include <Lavfi/Gfx/HwDevice.hpp>
#include <Lavfi/Gfx/VulkanTransport.hpp>

#include <QGuiApplication>
#include <QtGui/private/qrhi_p.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

using namespace score::gfx;

namespace
{
int g_fail = 0;
void check(bool ok, const char* what)
{
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok)
    ++g_fail;
}

std::vector<uint8_t> makePattern(int w, int h)
{
  std::vector<uint8_t> px(size_t(w) * h * 4);
  for(int y = 0; y < h; ++y)
    for(int x = 0; x < w; ++x)
    {
      uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
      // Smooth along x: FFmpeg 6.1's flip shader samples half a texel off,
      // so a horizontally noisy pattern would compare interpolated values.
      p[0] = uint8_t(x * 255 / (w - 1));
      p[1] = uint8_t(y * 4); // smooth along y as well, same reason
      p[2] = uint8_t(96 + y);
      p[3] = 255;
    }
  return px;
}

std::vector<uint8_t> hflip(const std::vector<uint8_t>& px, int w, int h)
{
  std::vector<uint8_t> out(px.size());
  for(int y = 0; y < h; ++y)
    for(int x = 0; x < w; ++x)
      std::memcpy(
          out.data() + (size_t(y) * w + x) * 4, px.data() + (size_t(y) * w + (w - 1 - x)) * 4,
          4);
  return out;
}

// One render frame: upload the pattern into the transport's current input
// image, submit; then hand it over, filter, present, read back.
bool roundTrip(
    const RenderState& state, Lavfi::VulkanTransport& vk, Lavfi::Graph& graph, int input,
    const std::vector<uint8_t>& px, int w, int h, std::vector<uint8_t>& out)
{
  auto& rhi = *state.rhi;
  if(!vk.acquireInput(state, input))
    return false;
  auto rt = vk.renderTargetForInput(input);
  if(!rt.texture)
    return false;

  // "Render" into the pool image: an upload is a transfer write, ordered
  // like a pass for our purposes, and exact.
  {
    QRhiCommandBuffer* cb{};
    rhi.beginOffscreenFrame(&cb);
    auto* b = rhi.nextResourceUpdateBatch();
    QRhiTextureSubresourceUploadDescription sub{
        QByteArray(reinterpret_cast<const char*>(px.data()), int(px.size()))};
    b->uploadTexture(rt.texture, QRhiTextureUploadDescription{{0, 0, sub}});
    cb->resourceUpdate(b);
    rhi.endOffscreenFrame();
  }

  // The frame becomes `rendered` at the next rotation, once its upload has
  // been submitted (endOffscreenFrame above did that).
  if(!vk.acquireInput(state, input))
    return false;
  AVFrame* f = vk.takeInput(input);
  if(!f)
    return false;
  f->pts = 0;
  const bool pushed = graph.pushVideo(0, f);
  av_frame_free(&f);
  if(!pushed)
    return false;
  AVFrame* o = graph.pullVideo(0);
  if(!o)
    return false;
  QRhiTexture* tex = vk.presentOutput(state, o);
  if(!tex)
    return false;

  QRhiReadbackResult rb;
  {
    QRhiCommandBuffer* cb{};
    rhi.beginOffscreenFrame(&cb);
    auto* b = rhi.nextResourceUpdateBatch();
    b->readBackTexture(QRhiReadbackDescription{tex}, &rb);
    cb->resourceUpdate(b);
    rhi.endOffscreenFrame();
  }
  if(rb.data.size() != int(px.size()))
    return false;
  out.assign(rb.data.begin(), rb.data.end());
  return true;
}

// A Vulkan QRhi on a VkDevice made like score's shared one (ScreenNode /
// VulkanVideoDevice.hpp: every feature the GPU reports, score's extension
// list) plus VK_EXT_descriptor_buffer, which FFmpeg 6.1's Vulkan filters
// insist on and 7.1+ no longer need. score::gfx::createRenderState needs the
// whole score application context; the transport only needs the device.
std::shared_ptr<RenderState> makeVulkanState(QSize sz)
{
#if QT_HAS_VULKAN && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  QVulkanInstance* inst = staticVulkanInstance();
  if(!inst)
    return nullptr;
  auto* funcs = inst->functions();

  VkPhysicalDevice physDev = resolveSharedVulkanPhysicalDevice(inst);
  if(physDev == VK_NULL_HANDLE)
    return nullptr;

  uint32_t qfCount = 0;
  funcs->vkGetPhysicalDeviceQueueFamilyProperties(physDev, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qfProps(qfCount);
  funcs->vkGetPhysicalDeviceQueueFamilyProperties(physDev, &qfCount, qfProps.data());
  uint32_t gfxFamily = UINT32_MAX;
  for(uint32_t i = 0; i < qfCount; i++)
    if(qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
    {
      gfxFamily = i;
      break;
    }
  if(gfxFamily == UINT32_MAX)
    return nullptr;

  uint32_t extCount = 0;
  funcs->vkEnumerateDeviceExtensionProperties(physDev, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> avail(extCount);
  funcs->vkEnumerateDeviceExtensionProperties(physDev, nullptr, &extCount, avail.data());
  auto hasExt = [&](const char* name) {
    for(auto& e : avail)
      if(std::strcmp(e.extensionName, name) == 0)
        return true;
    return false;
  };
  std::vector<const char*> exts;
  for(auto* e : sharedVulkanDeviceExtensions())
    if(hasExt(e))
      exts.push_back(e);
  bool descriptorBuffer = false;
#ifdef VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME
  if(hasExt(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME))
  {
    exts.push_back(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    descriptorBuffer = true;
  }
#endif

  auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
      inst->getInstanceProcAddr("vkGetPhysicalDeviceFeatures2"));
  if(!getFeatures2)
    return nullptr;
#ifdef VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME
  VkPhysicalDeviceDescriptorBufferFeaturesEXT descBuf{};
  descBuf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
#endif
  VkPhysicalDeviceVulkan13Features vk13{};
  vk13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
#ifdef VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME
  if(descriptorBuffer)
    vk13.pNext = &descBuf;
#endif
  VkPhysicalDeviceVulkan12Features vk12{};
  vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  vk12.pNext = &vk13;
  VkPhysicalDeviceVulkan11Features vk11{};
  vk11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  vk11.pNext = &vk12;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &vk11;
  getFeatures2(physDev, &features2);
  features2.features.robustBufferAccess = VK_FALSE;
  vk13.robustImageAccess = VK_FALSE;
  const bool timeline = vk12.timelineSemaphore == VK_TRUE;

  std::vector<VkDeviceQueueCreateInfo> queues;
  float priority = 1.f;
  for(uint32_t i = 0; i < qfCount; i++)
  {
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = i;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    queues.push_back(qi);
  }
  VkDeviceCreateInfo devInfo{};
  devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  devInfo.pNext = &features2;
  devInfo.queueCreateInfoCount = uint32_t(queues.size());
  devInfo.pQueueCreateInfos = queues.data();
  devInfo.enabledExtensionCount = uint32_t(exts.size());
  devInfo.ppEnabledExtensionNames = exts.data();
  auto createDevice
      = reinterpret_cast<PFN_vkCreateDevice>(inst->getInstanceProcAddr("vkCreateDevice"));
  VkDevice dev = VK_NULL_HANDLE;
  if(!createDevice || createDevice(physDev, &devInfo, nullptr, &dev) != VK_SUCCESS)
    return nullptr;
  VkQueue gfxQueue = VK_NULL_HANDLE;
  auto getQueue
      = reinterpret_cast<PFN_vkGetDeviceQueue>(inst->getInstanceProcAddr("vkGetDeviceQueue"));
  getQueue(dev, gfxFamily, 0, &gfxQueue);

  vkinterop::setDeviceTimelineSemaphoresEnabled(timeline);
#ifdef VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME
  if(descriptorBuffer)
    Lavfi::addVulkanDeviceExtensions({VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME});
#endif

  auto st = std::make_shared<RenderState>();
  auto& state = *st;
  QRhiVulkanInitParams params;
  params.inst = inst;
  QRhiVulkanNativeHandles h;
  h.physDev = physDev;
  h.dev = dev;
  h.gfxQueueFamilyIdx = gfxFamily;
  h.gfxQueueIdx = 0;
  h.gfxQueue = gfxQueue;
  h.inst = inst;
  state.rhi = QRhi::create(QRhi::Vulkan, &params, {}, &h);
  if(!state.rhi)
  {
    destroySharedVulkanDevice(inst, dev);
    return nullptr;
  }
  state.api = GraphicsApi::Vulkan;
  state.renderSize = sz;
  state.caps.populate(*state.rhi);
  state.customDeviceCleanup = [dev, inst] { destroySharedVulkanDevice(inst, dev); };
  std::printf("device: %s, descriptor_buffer=%d timeline=%d\n", "shared-like", int(descriptorBuffer), int(timeline));
  return st;
#else
  return nullptr;
#endif
}

void runTests()
{
  auto state = makeVulkanState(QSize(64, 32));
  if(!state || !state->rhi)
  {
    std::printf("SKIP: no Vulkan QRhi could be created\n");
    return;
  }
  auto& rhi = *state->rhi;
  std::printf("backend=%s\n", rhi.backendName());
  if(!Lavfi::VulkanTransport::available(rhi))
  {
    std::printf("SKIP: QRhi is not on score's shared VkDevice (timeline semaphores off)\n");
    return;
  }
  if(!Lavfi::hasFilter("hflip_vulkan"))
  {
    std::printf("SKIP: this FFmpeg has no Vulkan filters\n");
    return;
  }

  // Everything FFmpeg allocates on the device must be gone before the
  // device is: scope it.
  {
  AVBufferRef* dev = Lavfi::vulkanDeviceForRhi(rhi);
  check(dev != nullptr, "AVVulkanDeviceContext over QRhi's device");
  if(!dev)
    return;

  const int W = 64, H = 32;
  Lavfi::VulkanTransport vk;
  check(vk.init(*state, dev), "transport init");
  const int input = vk.addInput(QSize(W, H));
  check(input == 0, "input pool with COLOR_ATTACHMENT usage");
  if(input < 0)
    return;

  Lavfi::Graph graph;
  std::vector<Lavfi::InputConfig> in(1);
  in[0].type = AVMEDIA_TYPE_VIDEO;
  in[0].video.width = W;
  in[0].video.height = H;
  in[0].video.format = AV_PIX_FMT_VULKAN;
  in[0].video.hw_frames_ctx = vk.inputFramesContext(0);
  Lavfi::SinkConfig sinks;
  sinks.pix_fmts = {AV_PIX_FMT_VULKAN};
  std::string err;
  check(graph.init("hflip_vulkan", in, sinks, dev, err), "graph configured on Vulkan frames");
  if(!graph.valid())
  {
    std::printf("%s\n", err.c_str());
    return;
  }
  check(graph.outputPixelFormat(0) == AV_PIX_FMT_VULKAN, "sink produces Vulkan frames");

  const auto px = makePattern(W, H);
  const auto expected = hflip(px, W, H);
  // Several frames: exercises pool recycling, the output ring and the
  // per-image render target cache.
  for(int frame = 0; frame < 6; frame++)
  {
    std::vector<uint8_t> out;
    const bool ok = roundTrip(*state, vk, graph, 0, px, W, H, out);
    char what[64];
    std::snprintf(what, sizeof(what), "frame %d round trip", frame);
    check(ok, what);
    if(!ok)
      break;
    // hflip_vulkan samples with a half-texel offset in FFmpeg 6.1 (linear
    // interpolation between neighbours): allow the interpolation, which is
    // at most half the gradient step (4) here. Edges and the alpha byte are
    // exact either way.
    bool same = out.size() == expected.size();
    for(size_t i = 0; same && i < out.size(); i++)
      same = std::abs(int(out[i]) - int(expected[i])) <= 3;
    std::snprintf(what, sizeof(what), "frame %d pixels are the hflip of the input", frame);
    check(same, what);
    if(!same)
    {
      size_t bad = 0, first = out.size();
      int maxDiff = 0;
      for(size_t i = 0; i < out.size(); i++)
      {
        const int d = std::abs(int(out[i]) - int(expected[i]));
        maxDiff = std::max(maxDiff, d);
        if(d > 3)
        {
          bad++;
          if(first == out.size())
            first = i;
        }
      }
      std::printf("  max byte difference %d\n", maxDiff);
      if(first == out.size())
        first = 0;
      std::printf(
          "  %zu/%zu bytes differ by more than 3; first at px (%zu,%zu) got %d,%d,%d,%d want %d,%d,%d,%d\n", bad,
          out.size(), (first / 4) % W, (first / 4) / W, out[first & ~3u], out[(first & ~3u) + 1],
          out[(first & ~3u) + 2], out[(first & ~3u) + 3], expected[first & ~3u],
          expected[(first & ~3u) + 1], expected[(first & ~3u) + 2], expected[(first & ~3u) + 3]);
      break;
    }
  }

  // A CPU-only filter must be refused on Vulkan frames (that is what makes
  // the renderer fall back to the CPU transport), not silently mangled.
  {
    Lavfi::Graph bad;
    std::string e;
    check(!bad.init("negate", in, sinks, dev, e), "CPU filter on Vulkan frames is refused");
  }

  // hwdownload inside the graph works on the shared device too.
  {
    Lavfi::Graph mixed;
    Lavfi::SinkConfig cpuSinks;
    cpuSinks.pix_fmts = {AV_PIX_FMT_RGBA};
    std::string e;
    const bool ok = mixed.init("hwdownload,format=rgba,negate", in, cpuSinks, dev, e);
    check(ok, "hwdownload,format=rgba,negate configures");
    if(!ok)
      std::printf("%s\n", e.c_str());
  }

  vk.release();
  av_buffer_unref(&dev);
  }
  std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
  delete state->rhi;
  state->rhi = nullptr;
  if(state->customDeviceCleanup)
    state->customDeviceCleanup();
}
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  QLocale::setDefault(QLocale::C);
  std::setlocale(LC_ALL, "C");
  qputenv("SCORE_DISABLE_AUDIOPLUGINS", "1");
  qputenv("SCORE_AUDIO_BACKEND", "dummy");

  // A plain Qt application: createRenderState needs a QVulkanInstance and a
  // display, not score's plug-in stack.
  QGuiApplication app(argc, argv);
  runTests();
  return g_fail ? 1 : 0;
}
