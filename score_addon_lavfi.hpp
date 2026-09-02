#pragma once
#include <score/application/ApplicationContext.hpp>
#include <score/command/Command.hpp>
#include <score/command/CommandGeneratorMap.hpp>
#include <score/plugins/InterfaceList.hpp>
#include <score/plugins/qt_interfaces/CommandFactory_QtInterface.hpp>
#include <score/plugins/qt_interfaces/FactoryInterface_QtInterface.hpp>
#include <score/plugins/qt_interfaces/PluginRequirements_QtInterface.hpp>

#include <utility>
#include <vector>

/**
 * score-addon-lavfi: FFmpeg libavfilter graphs as score processes.
 *
 * One process, "FFmpeg filter", whose program is a filtergraph string. Its
 * ports are derived from the graph: one audio/texture port per open pad, one
 * control inlet per runtime-changeable filter option, and a metadata outlet.
 * Audio graphs run in the audio thread; video graphs run on the render thread
 * as a gfx node whose frame transport is GPU-resident (Vulkan) where the
 * backend and the filters allow it, and readback/upload otherwise.
 */
class score_addon_lavfi final
    : public score::Plugin_QtInterface
    , public score::FactoryInterface_QtInterface
    , public score::CommandFactory_QtInterface
{
  SCORE_PLUGIN_METADATA(1, "b2a6e2f4-8c1e-4d5c-a1a7-5f0d2e9c3b41")

public:
  score_addon_lavfi();
  ~score_addon_lavfi() override;

private:
  std::vector<score::InterfaceBase*> factories(
      const score::ApplicationContext& ctx, const score::InterfaceKey& key) const override;

  std::pair<const CommandGroupKey, CommandGeneratorMap> make_commands() override;

  std::vector<score::PluginKey> required() const override;
};
