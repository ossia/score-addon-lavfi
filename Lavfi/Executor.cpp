#include "Executor.hpp"

#include <Process/Dataflow/Port.hpp>
#include <Process/ExecutionContext.hpp>
#include <Process/ExecutionSetup.hpp>
#include <Process/ExecutionTransaction.hpp>

#include <score/document/DocumentContext.hpp>
#include <score/tools/Bind.hpp>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/port.hpp>

#include <Gfx/GfxApplicationPlugin.hpp>
#include <Gfx/GfxContext.hpp>
#include <Gfx/GfxExecNode.hpp>
#include <Gfx/TexturePort.hpp>
#include <Lavfi/AudioNode.hpp>
#include <Lavfi/Node.hpp>
#include <Lavfi/Process.hpp>

namespace Lavfi
{
namespace
{
/// Bridge for graphs with video pads: mirrors the model's ports, forwards
/// controls and audio to the GfxNode as messages (Gfx::gfx_exec_node::run).
class gfx_node final : public Gfx::gfx_exec_node
{
public:
  gfx_node(Gfx::GfxExecutionAction& ctx, const Lavfi::Model& model, int sampleRate)
      : gfx_exec_node{ctx}
  {
    GfxNode::Program program{
        model.script().toStdString(), model.description(), model.controlOptions(),
        sampleRate};

    for(auto* inlet : model.inlets())
    {
      if(qobject_cast<Gfx::TextureInlet*>(inlet))
        add_texture();
      else if(qobject_cast<Process::AudioInlet*>(inlet))
        add_audio();
      else
        add_control();
    }
    for(auto* outlet : model.outlets())
    {
      if(qobject_cast<Gfx::TextureOutlet*>(outlet))
        add_texture_out();
      else if(qobject_cast<Process::AudioOutlet*>(outlet))
        m_outlets.push_back(new ossia::audio_outlet); // audio out of a video graph: silent
      else
        add_control_out();
    }

    id = exec_context->ui->register_node(std::make_unique<GfxNode>(std::move(program)));
  }

  ~gfx_node() override { exec_context->ui->unregister_node(id); }

  std::string label() const noexcept override { return "lavfi"; }
};

std::shared_ptr<ossia::graph_node>
makeNode(const Lavfi::Model& model, const Execution::Context& ctx)
{
  const int rate = ctx.execState->sampleRate;
  if(!model.hasVideo())
    return std::make_shared<audio_node>(
        model.script().toStdString(), model.description(), model.controlOptions(), rate);
  return std::make_shared<gfx_node>(ctx.doc.plugin<Gfx::DocumentPlugin>().exec, model, rate);
}
}

ProcessExecutorComponent::ProcessExecutorComponent(
    Lavfi::Model& element, const Execution::Context& ctx, QObject* parent)
    : ProcessComponent_T{element, ctx, "LavfiExecutorComponent", parent}
{
  m_inletCount = element.inlets().size();
  m_outletCount = element.outlets().size();

  try
  {
    auto n = makeNode(element, ctx);
    this->node = n;
    m_ossia_process = std::make_shared<ossia::node_process>(n);
  }
  catch(...)
  {
    return;
  }

  // Live edits. Same port layout: audio nodes take the new program at their
  // next tick. Otherwise (or for video graphs) the node is replaced under the
  // running graph, the way the Faust process does it.
  con(element, &Lavfi::Model::programChanged, this,
      [this] {
    auto& m = this->process();
    auto& ctx = system();

    if(auto an = std::dynamic_pointer_cast<audio_node>(this->node);
       an && !m.hasVideo() && m.inlets().size() == m_inletCount
       && m.outlets().size() == m_outletCount)
    {
      // The exec queue's small-function storage is 128 bytes: ship the
      // program through the heap.
      struct Payload
      {
        std::string script;
        Lavfi::Description desc;
        std::vector<Lavfi::OptionInfo> controls;
      };
      auto payload = std::make_shared<Payload>(
          Payload{m.script().toStdString(), m.description(), m.controlOptions()});
      in_exec([an, payload] {
        an->setProgram(
            std::move(payload->script), std::move(payload->desc),
            std::move(payload->controls));
      });
      return;
    }

    Execution::SetupContext& setup = ctx.setup;
    auto old_node = this->node;
    Execution::Transaction commands{ctx};
    if(old_node)
      setup.unregister_node(m, old_node);

    std::shared_ptr<ossia::graph_node> n;
    try
    {
      n = makeNode(m, ctx);
    }
    catch(...)
    {
    }
    this->node = n;
    m_inletCount = m.inlets().size();
    m_outletCount = m.outlets().size();
    if(n)
    {
      setup.register_node(m, n);
      nodeChanged(old_node, n, &commands);
    }
    commands.run_all();
  },
      Qt::DirectConnection);
}

ProcessExecutorComponent::~ProcessExecutorComponent() = default;
}
