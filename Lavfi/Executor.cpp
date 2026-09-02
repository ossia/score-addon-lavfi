#include "Executor.hpp"

#include <Process/Dataflow/Port.hpp>
#include <Process/ExecutionContext.hpp>

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
  gfx_node(Gfx::GfxExecutionAction& ctx, const Lavfi::Model& model)
      : gfx_exec_node{ctx}
  {
    GfxNode::Program program{
        model.script().toStdString(), model.description(), model.controlOptions()};

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
}

ProcessExecutorComponent::ProcessExecutorComponent(
    Lavfi::Model& element, const Execution::Context& ctx, QObject* parent)
    : ProcessComponent_T{element, ctx, "LavfiExecutorComponent", parent}
{
  m_inletCount = element.inlets().size();
  m_outletCount = element.outlets().size();

  if(!element.hasVideo())
  {
    auto n = std::make_shared<audio_node>(
        element.script().toStdString(), element.description(), element.controlOptions(),
        ctx.execState->sampleRate);
    this->node = n;
    m_ossia_process = std::make_shared<ossia::node_process>(n);

    // Same port layout, new program: swap it in at the next tick.
    con(element, &Lavfi::Model::programChanged, this, [this, weak = std::weak_ptr{n}] {
      auto n = weak.lock();
      if(!n)
        return;
      auto& m = this->process();
      if(m.inlets().size() != m_inletCount || m.outlets().size() != m_outletCount)
        return;
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
      in_exec([n, payload] {
        n->setProgram(
            std::move(payload->script), std::move(payload->desc),
            std::move(payload->controls));
      });
    });
  }
  else
  {
    try
    {
      auto n = std::make_shared<gfx_node>(
          ctx.doc.plugin<Gfx::DocumentPlugin>().exec, element);
      this->node = n;
      m_ossia_process = std::make_shared<ossia::node_process>(n);
    }
    catch(...)
    {
    }
  }
}

ProcessExecutorComponent::~ProcessExecutorComponent() = default;
}
