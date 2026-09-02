#pragma once
#include <Process/Script/ScriptEditor.hpp>

#include <Effect/EffectFactory.hpp>

#include <Lavfi/Process.hpp>

namespace Lavfi
{
struct LanguageSpec
{
  // Picks the highlighter in Process::createScriptWidget; there is no
  // filtergraph mode, the default (C-like) one does fine for key=value:...
  static constexpr const char* language = "lavfi";
};

using LayerFactory = Process::ScriptLayerFactory_T<
    Lavfi::Model,
    Process::ProcessScriptEditDialog<Lavfi::Model, Lavfi::Model::p_script, LanguageSpec>>;
}
