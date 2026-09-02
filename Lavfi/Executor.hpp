#pragma once
#include <Process/Execution/ProcessComponent.hpp>

#include <ossia/dataflow/node_process.hpp>

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
 * one to one, which is what score's cable wiring relies on. Editing the
 * graph while playing changes the program of the running node when its
 * port layout is unchanged; a layout change takes effect on the next play.
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
  std::size_t m_inletCount{}, m_outletCount{};
};

using ProcessExecutorComponentFactory
    = Execution::ProcessComponentFactory_T<ProcessExecutorComponent>;
}
