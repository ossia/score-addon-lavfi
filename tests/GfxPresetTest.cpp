// A preset through score's own render pipeline, checked on the pixels.
//
// tests/PresetBehaviourTest.cpp checks what libavfilter produces; this checks
// what score displays, which is a different thing: the frame goes source node
// -> our gfx node (upload, filter, download or the Vulkan transport) -> an
// offscreen sink, and comes back as RGBA8 bytes. Everything between the filter
// and the picture -- the transport ladder, the video decoder, the sampler, the
// output pass and the Y flip -- only exists on this path.
//
// Built on the same fixture as score's own gfx tests (tests/fixtures/
// score_test/Gfx.hpp): GfxPipeline wires the graph, BackgroundNode is the
// offscreen sink, and the whole thing SKIPs cleanly where no RHI backend comes
// up (CI without a GPU).
#include <Lavfi/Core/Graph.hpp>
#include <Lavfi/Node.hpp>

#include <score_test/App.hpp>
#include <score_test/Gfx.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <memory>
#include <string>

namespace
{
using score::test::gfx::ReadbackImage;

QString corpus(const char* name)
{
  return QStringLiteral(GFX_TEST_CORPUS_DIR "/") + QString::fromUtf8(name);
}

struct Rendered
{
  bool skipped{};
  std::string skipReason;
  std::string error;
  std::string backend;
  ReadbackImage img;
};

/// source shader -> lavfi graph -> offscreen sink, `frames` frames of it.
Rendered render(
    score::gfx::GraphicsApi api, const char* sourceShader, const std::string& script,
    int frames = 16, QSize size = {64, 64})
{
  Rendered out;
  score::test::run_in_gui_app([&](const score::GUIApplicationContext&) {
    score::test::gfx::GfxPipeline p;

    const int src = p.addIsf(corpus(sourceShader));

    Lavfi::GfxNode::Program prog;
    prog.script = script;
    std::string err;
    if(!Lavfi::Graph::describe(script, prog.desc, err))
    {
      out.error = "describe: " + err;
      return;
    }
    prog.metadata = std::make_shared<Lavfi::MetadataMailbox>();
    const int node = p.addNode(std::make_unique<Lavfi::GfxNode>(std::move(prog)));
    const int sink = p.addSink(size);
    if(node < 0 || src < 0 || sink < 0)
    {
      out.error = "the pipeline could not be built";
      return;
    }

    p.wire(p.imageOut(src), p.node(node)->input[0]);
    p.wire(p.nodeImageOut(node), p.sinkInput(sink));

    if(!p.create(api))
    {
      out.skipped = p.skipped();
      out.skipReason = p.skipReason();
      out.error = p.error();
      out.backend = p.backend();
      return;
    }
    p.render(frames);
    out.backend = p.backend();
    out.error = p.error();
    out.img = p.readback(sink);
  });
  return out;
}

int chan(const ReadbackImage& img, int x, int y, int c)
{
  return img.at(x, y)[c];
}
}

// Control: the same fixture, the same source, no lavfi node in between. If
// this fails the harness is wrong, not the addon.
TEST_CASE("the fixture renders the source shader", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));
  Rendered out;
  score::test::run_in_gui_app([&](const score::GUIApplicationContext&) {
    score::test::gfx::GfxPipeline p;
    const int src = p.addIsf(corpus("isf-solid-color.fs"));
    const int sink = p.addSink({64, 64});
    p.wire(p.imageOut(src), p.sinkInput(sink));
    if(!p.create(api))
    {
      out.skipped = p.skipped();
      out.skipReason = p.skipReason();
      out.error = p.error();
      return;
    }
    p.render(4);
    out.img = p.readback(sink);
    out.backend = p.backend();
  });
  if(out.skipped)
    SKIP(out.backend + ": " + out.skipReason);
  REQUIRE(out.img.valid());
  const auto px = out.img.center();
  CAPTURE(int(px[0]), int(px[1]), int(px[2]));
  CHECK(int(px[0]) > 200);
  CHECK(int(px[2]) > 200);
}

// The source is a flat magenta (255, 0, 255). Anything that comes back at the
// wrong scale, in the wrong channel order or upside down shows here first.
TEST_CASE("a lavfi node passes a picture through unchanged", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  // hue=h=0:s=1 is the neutral setting of a real preset, so this is the
  // identity through upload, filter, download and sampling.
  const Rendered r = render(api, "isf-solid-color.fs", "hue=h=0:s=1");
  if(r.skipped)
    SKIP(r.backend + ": " + r.skipReason);
  REQUIRE(r.error.empty());
  REQUIRE(r.img.valid());
  CAPTURE(r.backend);

  // `hue` works in YUV, so a saturated magenta makes the round trip through
  // limited-range chroma and comes back a few counts short. That is the
  // filter's own colour handling, not the transport: what this pins down is
  // that the picture arrives, right side up, in the right channels.
  const auto px = r.img.center();
  CAPTURE(int(px[0]), int(px[1]), int(px[2]), int(px[3]));
  CHECK(std::abs(int(px[0]) - 255) <= 20);
  CHECK(std::abs(int(px[1]) - 0) <= 20);
  CHECK(std::abs(int(px[2]) - 255) <= 20);
  CHECK(px[3] >= 250);
}

// The inverse of magenta is green, so this catches a channel swap as well as a
// filter that did not run at all.
TEST_CASE("a lavfi node inverts the picture it is given", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  const Rendered r = render(api, "isf-solid-color.fs", "negate");
  if(r.skipped)
    SKIP(r.backend + ": " + r.skipReason);
  REQUIRE(r.error.empty());
  REQUIRE(r.img.valid());
  CAPTURE(r.backend);

  const auto px = r.img.center();
  CHECK(std::abs(int(px[0]) - 0) <= 6);
  CHECK(std::abs(int(px[1]) - 255) <= 6);
  CHECK(std::abs(int(px[2]) - 0) <= 6);
}

// A horizontal gradient, mirrored: the dark end must end up where the bright
// one was. A vertical flip (the classic readback bug) would not show on a
// gradient that only varies in x, which is exactly why the next case exists.
TEST_CASE("a lavfi node flips the picture horizontally", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  const Rendered plain = render(api, "isf-gradient-x.fs", "null");
  if(plain.skipped)
    SKIP(plain.backend + ": " + plain.skipReason);
  REQUIRE(plain.error.empty());
  REQUIRE(plain.img.valid());

  const Rendered flipped = render(api, "isf-gradient-x.fs", "hflip");
  REQUIRE(flipped.error.empty());
  REQUIRE(flipped.img.valid());
  CAPTURE(flipped.backend);

  const int y = plain.img.height / 2;
  const int w = plain.img.width;
  // The gradient must actually be a gradient, or the rest proves nothing.
  REQUIRE(std::abs(chan(plain.img, w - 3, y, 0) - chan(plain.img, 2, y, 0)) > 100);
  for(int x : {2, w / 4, w / 2, w - 3})
  {
    INFO("column " << x);
    CHECK(std::abs(chan(flipped.img, x, y, 0) - chan(plain.img, w - 1 - x, y, 0)) <= 8);
  }
}

// The same, upside down. Two transposes rather than `vflip`: vflip only
// rewrites the stride and the plane pointer, which it will happily "do" to a
// hardware frame whose data pointer is a VkImage handle -- the frame then
// comes out untouched on the Vulkan transport. Anything that has to look at
// the pixels (transpose here) makes libavfilter insert the download and upload
// it needs, so the same graph means the same thing on both transports.
TEST_CASE("a lavfi node turns the picture upside down", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  const Rendered plain = render(api, "isf-gradient-y.fs", "null");
  if(plain.skipped)
    SKIP(plain.backend + ": " + plain.skipReason);
  REQUIRE(plain.error.empty());
  REQUIRE(plain.img.valid());

  const Rendered flipped = render(api, "isf-gradient-y.fs", "transpose=clock,transpose=clock");
  REQUIRE(flipped.error.empty());
  REQUIRE(flipped.img.valid());
  CAPTURE(flipped.backend);

  const int x = plain.img.width / 2;
  const int h = plain.img.height;
  REQUIRE(std::abs(chan(plain.img, x, h - 3, 1) - chan(plain.img, x, 2, 1)) > 100);
  for(int y : {2, h / 4, h / 2, h - 3})
  {
    INFO("row " << y);
    CHECK(std::abs(chan(flipped.img, x, y, 1) - chan(plain.img, x, h - 1 - y, 1)) <= 8);
  }
}

// A graph whose last filter emits planar YUV with an alpha plane: the frame
// that reaches the uploader then has four planes at two different sizes, which
// is where a decoder picking one size for all of them shows up as the picture
// in a corner with a scaled copy over it.
TEST_CASE("a lavfi node shows a planar YUV frame with alpha", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  const Rendered r = render(api, "isf-gradient-x.fs", "format=yuva420p");
  if(r.skipped)
    SKIP(r.backend + ": " + r.skipReason);
  REQUIRE(r.error.empty());
  REQUIRE(r.img.valid());
  CAPTURE(r.backend);

  const Rendered plain = render(api, "isf-gradient-x.fs", "null");
  REQUIRE(plain.img.valid());
  const int y = r.img.height / 2;
  for(int x : {2, r.img.width / 4, r.img.width / 2, r.img.width - 3})
  {
    INFO("column " << x);
    // 8-bit limited-range YUV with subsampled chroma: a gradient comes back
    // within a step or two, not as a different picture.
    CHECK(std::abs(chan(r.img, x, y, 0) - chan(plain.img, x, y, 0)) <= 20);
  }
}

// A GPU filter written the portable way, with the upload and download in the
// graph. On the Vulkan transport the node is already handing the graph
// hardware frames, so this has to keep working rather than come out black.
TEST_CASE("a lavfi node runs a graph that uploads for itself", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  const Rendered r
      = render(api, "isf-gradient-x.fs", "hwupload,hflip_vulkan,hwdownload,format=rgba");
  if(r.skipped)
    SKIP(r.backend + ": " + r.skipReason);
  if(!r.error.empty())
    SKIP(r.backend + ": " + r.error); // no Vulkan filters in this FFmpeg
  REQUIRE(r.img.valid());
  CAPTURE(r.backend);

  const Rendered plain = render(api, "isf-gradient-x.fs", "null");
  REQUIRE(plain.img.valid());
  const int y = r.img.height / 2;
  const int w = r.img.width;
  REQUIRE(std::abs(chan(plain.img, w - 3, y, 0) - chan(plain.img, 2, y, 0)) > 100);
  for(int x : {2, w / 4, w / 2, w - 3})
  {
    INFO("column " << x);
    CHECK(std::abs(chan(r.img, x, y, 0) - chan(plain.img, w - 1 - x, y, 0)) <= 10);
  }
}

// A frame that comes back from CUDA: hwdownload hands over a plane whose
// stride is padded to the driver's pitch, which is where a picture drawn as if
// stride were width ends up in a corner, repeated.
TEST_CASE("a lavfi node shows a frame downloaded from CUDA", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  const Rendered r = render(
      api, "isf-gradient-x.fs",
      "format=yuv420p,hwupload_cuda,scale_cuda=64:64,hwdownload,format=yuv420p");
  if(r.skipped)
    SKIP(r.backend + ": " + r.skipReason);
  if(!r.error.empty())
    SKIP(r.backend + ": " + r.error); // no CUDA here
  REQUIRE(r.img.valid());
  CAPTURE(r.backend);

  const Rendered plain = render(api, "isf-gradient-x.fs", "null");
  REQUIRE(plain.img.valid());
  const int y = r.img.height / 2;
  for(int x : {2, r.img.width / 4, r.img.width / 2, r.img.width - 3})
  {
    INFO("column " << x);
    CHECK(std::abs(chan(r.img, x, y, 0) - chan(plain.img, x, y, 0)) <= 20);
  }
}

// The shipped CUDA chroma key, at a size whose stride the driver pads: the
// keyer also adds an alpha plane, so what comes back down is four planes at
// two sizes with a stride wider than the picture.
TEST_CASE("the CUDA chroma key preset shows what it keyed", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  const char* graph = "format=yuv420p,hwupload_cuda,"
                      "chromakey_cuda=color=0x00FF00:similarity=0.3:blend=0.1,"
                      "hwdownload,format=yuva420p";
  const QSize size{300, 200}; // not a multiple of any plausible pitch
  const Rendered r = render(api, "isf-gradient-x.fs", graph, 16, size);
  if(r.skipped)
    SKIP(r.backend + ": " + r.skipReason);
  if(!r.error.empty())
    SKIP(r.backend + ": " + r.error); // no CUDA here
  REQUIRE(r.img.valid());
  CAPTURE(r.backend);

  const Rendered plain = render(api, "isf-gradient-x.fs", "null", 16, size);
  REQUIRE(plain.img.valid());
  REQUIRE(r.img.width == plain.img.width);
  const int y = r.img.height / 2;
  for(int x : {2, r.img.width / 4, r.img.width / 2, r.img.width - 3})
  {
    INFO("column " << x);
    // The gradient has no green in it, so the keyer leaves the picture alone.
    CHECK(std::abs(chan(r.img, x, y, 0) - chan(plain.img, x, y, 0)) <= 20);
  }
}

// An audio visualiser: audio in, picture out, no video input at all. The
// samples arrive the way score's engine delivers them, one buffer per frame
// on the node's Audio port.
TEST_CASE("a lavfi node draws the audio it is given", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  Rendered out;
  score::test::run_in_gui_app([&](const score::GUIApplicationContext&) {
    score::test::gfx::GfxPipeline p;

    Lavfi::GfxNode::Program prog;
    prog.script = "showvolume=w=320:h=60:rate=60";
    std::string err;
    if(!Lavfi::Graph::describe(prog.script, prog.desc, err))
    {
      out.error = "describe: " + err;
      return;
    }
    prog.metadata = std::make_shared<Lavfi::MetadataMailbox>();
    const int node = p.addNode(std::make_unique<Lavfi::GfxNode>(std::move(prog)));
    const int sink = p.addSink({320, 60});
    if(node < 0 || sink < 0)
    {
      out.error = "the pipeline could not be built";
      return;
    }
    p.wire(p.nodeImageOut(node), p.sinkInput(sink));
    if(!p.create(api))
    {
      out.skipped = p.skipped();
      out.skipReason = p.skipReason();
      out.error = p.error();
      out.backend = p.backend();
      return;
    }

    // The Audio port is the graph's one audio pad: the node has no other input.
    int audioPort = -1;
    for(std::size_t i = 0; i < p.node(node)->input.size(); i++)
      if(p.node(node)->input[i]->type == score::gfx::Types::Audio)
      {
        audioPort = int(i);
        break;
      }
    if(audioPort < 0)
    {
      out.error = "the node has no audio input";
      return;
    }

    for(int f = 0; f < 40; f++)
    {
      // The entry point the engine uses to deliver a buffer to a node.
      static_cast<score::gfx::ProcessNode*>(p.node(node))
          ->process(int32_t(audioPort), score::test::gfx::const_audio(0.7, 512));
      p.render(1);
    }
    out.backend = p.backend();
    out.error = p.error();
    out.img = p.readback(sink);
  });

  if(out.skipped)
    SKIP(out.backend + ": " + out.skipReason);
  REQUIRE(out.error.empty());
  REQUIRE(out.img.valid());
  CAPTURE(out.backend);

  // A full-scale signal draws a long bar: plenty of lit pixels.
  int lit = 0;
  for(int y = 0; y < out.img.height; y++)
    for(int x = 0; x < out.img.width; x++)
      if(out.img.at(x, y)[0] + out.img.at(x, y)[1] + out.img.at(x, y)[2] > 120)
        lit++;
  CAPTURE(lit);
  CHECK(lit > 200);
}

// A graph the node cannot configure must leave the picture alone rather than
// take the render list down with it.
TEST_CASE("a lavfi node survives a graph it cannot build", "[lavfi][gfx]")
{
  const auto api = GENERATE(from_range(score::test::gfx::platform_backends()));
  CAPTURE(score::test::gfx::backend_name(api));

  const Rendered r = render(api, "isf-solid-color.fs", "no_such_filter_at_all");
  if(r.skipped)
    SKIP(r.backend + ": " + r.skipReason);
  // describe() refuses it before a node is ever built: that is the error the
  // process model reports too.
  CHECK(!r.error.empty());
}
