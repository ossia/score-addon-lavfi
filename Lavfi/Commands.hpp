#pragma once
#include <Scenario/Commands/ScriptEditCommand.hpp>

#include <Lavfi/Process.hpp>

namespace Lavfi
{
inline const CommandGroupKey& CommandFactoryName()
{
  static const CommandGroupKey key{"Lavfi"};
  return key;
}

/// Undoable graph edit that saves and restores the cables of the ports the
/// new graph drops (Scenario::EditScript does the port/cable bookkeeping).
class EditScript : public Scenario::EditScript<Lavfi::Model, Lavfi::Model::p_script>
{
  SCORE_COMMAND_DECL(CommandFactoryName(), EditScript, "Edit an FFmpeg filter graph")
public:
  using Scenario::EditScript<Lavfi::Model, Lavfi::Model::p_script>::EditScript;
};
}

namespace score
{
// The script dialog submits a StaticPropertyCommand<p_script>; route it to
// the cable-preserving command above.
template <>
struct StaticPropertyCommand<Lavfi::Model::p_script> : Lavfi::EditScript
{
  using Lavfi::EditScript::EditScript;
};
}
