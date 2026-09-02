#include "score_addon_lavfi.hpp"

#include <score/plugins/FactorySetup.hpp>

#include <ossia/detail/typelist.hpp>

#include <Lavfi/Commands.hpp>
#include <Lavfi/Executor.hpp>
#include <Lavfi/Layer.hpp>
#include <Lavfi/Process.hpp>
#include <score_addon_lavfi_commands_files.hpp>
#include <score_plugin_engine.hpp>
#include <score_plugin_gfx.hpp>
#include <score_plugin_media.hpp>
#include <score_plugin_scenario.hpp>

score_addon_lavfi::score_addon_lavfi() { }

score_addon_lavfi::~score_addon_lavfi() { }

std::vector<score::InterfaceBase*> score_addon_lavfi::factories(
    const score::ApplicationContext& ctx, const score::InterfaceKey& key) const
{
  return instantiate_factories<
      score::ApplicationContext, FW<Process::ProcessModelFactory, Lavfi::ProcessFactory>,
      FW<Process::LayerFactory, Lavfi::LayerFactory>,
      FW<Execution::ProcessComponentFactory, Lavfi::ProcessExecutorComponentFactory>>(
      ctx, key);
}

std::pair<const CommandGroupKey, CommandGeneratorMap> score_addon_lavfi::make_commands()
{
  using namespace Lavfi;
  std::pair<const CommandGroupKey, CommandGeneratorMap> cmds{
      CommandFactoryName(), CommandGeneratorMap{}};

  ossia::for_each_type<
#include <score_addon_lavfi_commands.hpp>
      >(score::commands::FactoryInserter{cmds.second});

  return cmds;
}

auto score_addon_lavfi::required() const -> std::vector<score::PluginKey>
{
  return {
      score_plugin_engine::static_key(), score_plugin_scenario::static_key(),
      score_plugin_media::static_key(), score_plugin_gfx::static_key()};
}

#include <score/plugins/PluginInstances.hpp>
SCORE_EXPORT_PLUGIN(score_addon_lavfi)
