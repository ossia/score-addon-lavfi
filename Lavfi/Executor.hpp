#pragma once
#include <Process/Execution/ProcessComponent.hpp>

#include <ossia/dataflow/node_process.hpp>

#include <QMetaObject>

#include <string>
#include <vector>

namespace Lavfi
{
class Model;

/**
 * @brief Picks the execution backend for a Lavfi::Model.
 *
 *  - audio-only graph : Lavfi::audio_node, runs in the audio thread.
 *  - graph with video : a Gfx::gfx_exec_node bridging to Lavfi::GfxNode on
 *                       the render thread (see Node.hpp for the transports).
 *
 * The port layout of the execution node mirrors the model's inlets/outlets
 * one to one, which is what score's cable wiring relies on. Control inlets
 * are wired to the node the way the ISF (gfx) and Faust (audio) executors
 * do it. Editing the graph while playing: same pad/control layout on an
 * audio graph swaps the program in place; anything else replaces the node
 * under the running graph (unregister, replace_node, register, nodeChanged).
 */
class ProcessExecutorComponent final
    : public Execution::ProcessComponent_T<Lavfi::Model, ossia::node_process>
{
  COMPONENT_METADATA("7c3d9a2e-4b5f-4e1a-8f6c-0d2e5b7a9c31")
public:
  ProcessExecutorComponent(
      Model& element, const Execution::Context& ctx, QObject* parent);
  ~ProcessExecutorComponent() override;

private:
  std::shared_ptr<ossia::graph_node> makeNode();
  void wireControls(const std::shared_ptr<ossia::graph_node>& node);

  std::string m_signature;
  std::vector<QMetaObject::Connection> m_controlConnections;
  int m_generation{};
};

using ProcessExecutorComponentFactory
    = Execution::ProcessComponentFactory_T<ProcessExecutorComponent>;
}
