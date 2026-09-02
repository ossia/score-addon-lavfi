#include "Executor.hpp"

#include <Process/Dataflow/Port.hpp>
#include <Process/ExecutionContext.hpp>
#include <Process/ExecutionSetup.hpp>
#include <Process/ExecutionTransaction.hpp>

#include <score/document/DocumentContext.hpp>
#include <score/tools/Bind.hpp>

#include <ossia/dataflow/execution_state.hpp>
#include <ossia/dataflow/port.hpp>
#include <ossia/network/value/value_conversion.hpp>

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
/// Controls are built the way the ISF executor does it: seeded with the
/// inlet's value, wired to valueChanged by the component afterwards.
class gfx_node final : public Gfx::gfx_exec_node
{
public:
  gfx_node(
      Gfx::GfxExecutionAction& ctx, const Lavfi::Model& model, int sampleRate,
      QObject* execContext, std::weak_ptr<Execution::ExecutionCommandQueue> queue)
      : gfx_exec_node{ctx}
  {
    GfxNode::Program program{
        model.script().toStdString(), model.description(), model.controlOptions(),
        sampleRate};
    program.queue = std::move(queue);

    for(auto* inlet : model.inlets())
    {
      if(qobject_cast<Gfx::TextureInlet*>(inlet))
        add_texture();
      else if(qobject_cast<Process::AudioInlet*>(inlet))
        add_audio();
      else if(auto ctrl = qobject_cast<Process::ControlInlet*>(inlet))
      {
        auto port = new ossia::value_inlet;
        m_inlets.push_back(port);
        auto control = std::make_shared<control_type>();
        control->port = &**port;
        control->value = ctrl->value();
        control->changed = true;
        controls.push_back(control);
        ctrl->setupExecution(*port, execContext);
      }
      else
        add_value_port();
    }
    for(auto* outlet : model.outlets())
    {
      if(qobject_cast<Gfx::TextureOutlet*>(outlet))
        add_texture_out();
      else if(qobject_cast<Process::AudioOutlet*>(outlet))
        m_outlets.push_back(new ossia::audio_outlet); // audio out of a video graph: silent
      else
        program.metadataOut = add_control_out();
    }

    id = exec_context->ui->register_node(std::make_unique<GfxNode>(std::move(program)));
  }
  using control_type = Gfx::exec_control;

  ~gfx_node() override { exec_context->ui->unregister_node(id); }

  std::string label() const noexcept override { return "lavfi"; }
};

/// Pad kinds and control count: what the execution node's port layout is
/// made of. Same signature = the running node can take the new program.
std::string layoutSignature(const Lavfi::Model& m)
{
  std::string s;
  for(auto& p : m.description().inputs)
    s += p.type == AVMEDIA_TYPE_AUDIO ? 'a' : p.type == AVMEDIA_TYPE_VIDEO ? 'v' : '?';
  s += '|';
  for(auto& p : m.description().outputs)
    s += p.type == AVMEDIA_TYPE_AUDIO ? 'a' : p.type == AVMEDIA_TYPE_VIDEO ? 'v' : '?';
  s += '|' + std::to_string(m.controlOptions().size());
  s += '|' + std::to_string(m.inlets().size()) + '/' + std::to_string(m.outlets().size());
  return s;
}
}

std::shared_ptr<ossia::graph_node> ProcessExecutorComponent::makeNode()
{
  auto& model = process();
  auto& ctx = system();
  const int rate = ctx.execState->sampleRate;
  if(!model.hasVideo())
    return std::make_shared<audio_node>(
        model.script().toStdString(), model.description(), model.controlOptions(), rate);
  return std::make_shared<gfx_node>(
      ctx.doc.plugin<Gfx::DocumentPlugin>().exec, model, rate, this,
      ctx.weakExecutionQueue());
}

void ProcessExecutorComponent::wireControls(const std::shared_ptr<ossia::graph_node>& n)
{
  for(auto& c : m_controlConnections)
    QObject::disconnect(c);
  m_controlConnections.clear();
  ++m_generation;

  auto& model = process();
  auto& ctx = system();

  if(auto gfx = std::dynamic_pointer_cast<gfx_node>(n))
  {
    // ISF pattern: the value goes through Gfx::con_unvalidated into the
    // control slot the exec node reads at its next tick.
    std::weak_ptr<Gfx::gfx_exec_node> weak = gfx;
    std::size_t control_index = 0;
    for(auto* inlet : model.inlets())
    {
      auto ctrl = qobject_cast<Process::ControlInlet*>(inlet);
      if(!ctrl)
        continue;
      m_controlConnections.push_back(QObject::connect(
          ctrl, &Process::ControlInlet::valueChanged, this,
          Gfx::con_unvalidated{ctx, control_index, gfx->script_index, weak}));
      control_index++;
    }
    return;
  }

  if(auto an = std::dynamic_pointer_cast<audio_node>(n))
  {
    // Faust pattern: write the value into the exec inlet from the execution
    // queue; the initial value likewise, so the first tick sees the UI state.
    an->generation = m_generation;
    std::weak_ptr<audio_node> weak = an;
    const auto& inputs = an->root_inputs();
    std::size_t i = 0;
    for(auto* inlet : model.inlets())
    {
      const std::size_t idx = i++;
      auto ctrl = qobject_cast<Process::ControlInlet*>(inlet);
      if(!ctrl || idx >= inputs.size())
        continue;
      ossia::inlet* inl = inputs[idx];
      auto vp = inl->target<ossia::value_port>();
      if(!vp)
        continue;
      vp->type = ctrl->value().get_type();
      vp->domain = ctrl->domain().get();
      auto push = [this, weak, inl, gen = m_generation](const ossia::value& v) {
        if(auto node = weak.lock())
          system().executionQueue.enqueue([inl, weak, val = v, gen]() mutable {
            if(auto n = weak.lock())
              if(gen == n->generation)
                inl->target<ossia::value_port>()->write_value(std::move(val), 0);
          });
      };
      push(ctrl->value());
      m_controlConnections.push_back(
          QObject::connect(ctrl, &Process::ControlInlet::valueChanged, this, push));
    }
  }
}

ProcessExecutorComponent::ProcessExecutorComponent(
    Lavfi::Model& element, const Execution::Context& ctx, QObject* parent)
    : ProcessComponent_T{element, ctx, "LavfiExecutorComponent", parent}
{
  try
  {
    auto n = makeNode();
    this->node = n;
    m_ossia_process = std::make_shared<ossia::node_process>(n);
    m_signature = layoutSignature(element);
    wireControls(n);
  }
  catch(...)
  {
    return;
  }

  // Live edits. Same port layout on an audio graph: the node takes the new
  // program at its next tick. Otherwise the node is replaced under the
  // running graph, the way the Faust process does it.
  con(element, &Lavfi::Model::programChanged, this,
      [this] {
    auto& m = this->process();
    auto& ctx = system();
    const std::string signature = layoutSignature(m);

    if(auto an = std::dynamic_pointer_cast<audio_node>(this->node);
       an && !m.hasVideo() && signature == m_signature)
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
      setup.unregister_node(m, old_node, commands);

    std::shared_ptr<ossia::graph_node> n;
    try
    {
      n = makeNode();
    }
    catch(...)
    {
    }
    this->node = n;
    m_signature = signature;
    if(n)
    {
      // The time process must drive the new node, not the old one.
      if(!m_ossia_process)
        m_ossia_process = std::make_shared<ossia::node_process>(n);
      else
        setup.replace_node(m_ossia_process, n, commands);
      setup.register_node(m, n, commands);
      wireControls(n);
      nodeChanged(old_node, n, &commands);
    }
    commands.run_all();
  },
      Qt::DirectConnection);
}

ProcessExecutorComponent::~ProcessExecutorComponent()
{
  for(auto& c : m_controlConnections)
    QObject::disconnect(c);
}
}
