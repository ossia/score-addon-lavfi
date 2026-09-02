#pragma once
#include <Process/Process.hpp>
#include <Process/Script/ScriptProcess.hpp>

#include <Effect/EffectFactory.hpp>

#include <Lavfi/Core/Graph.hpp>
#include <Lavfi/Metadata.hpp>

#include <verdigris>

namespace Lavfi
{
/**
 * @brief The "FFmpeg filter" process.
 *
 * The program is a libavfilter graph string (the same syntax as ffmpeg's
 * -filter_complex). Setting it re-derives the ports:
 *
 *   inlets : one audio inlet per open audio input pad, one texture inlet per
 *            open video input pad, then one control inlet per option flagged
 *            AV_OPT_FLAG_RUNTIME_PARAM on any filter instance of the graph
 *            (named "<filter>/<option>", forwarded with avfilter_process_command)
 *   outlets: one audio outlet per open audio output pad, one texture outlet
 *            per open video output pad, then a "Metadata" value outlet that
 *            carries the lavfi.* frame metadata analyzers publish.
 *
 * Existing ports whose name and kind match are kept, so cables survive edits
 * of the graph that do not touch them (the script command handles the rest).
 */
class Model final : public Process::ProcessModel
{
  W_OBJECT(Model)
  SCORE_SERIALIZE_FRIENDS
  PROCESS_METADATA_IMPL(Lavfi::Model)

public:
  Model(
      const TimeVal& duration, const QString& graph, const Id<Process::ProcessModel>& id,
      QObject* parent);

  template <typename Impl>
  Model(Impl& vis, QObject* parent)
      : Process::ProcessModel{vis, parent}
  {
    vis.writeTo(*this);
    init();
  }

  ~Model() override;

  QString prettyName() const noexcept override;

  bool validate(const QString& txt) const noexcept;
  const QString& script() const noexcept { return m_script; }
  [[nodiscard]] Process::ScriptChangeResult setScript(const QString& txt);

  /// Result of the last successful parse of script().
  const Lavfi::Description& description() const noexcept { return m_desc; }
  bool hasVideo() const noexcept { return m_desc.hasVideo(); }

  /// Options that became control inlets, in inlet order (after the pad inlets).
  const std::vector<Lavfi::OptionInfo>& controlOptions() const noexcept
  {
    return m_controls;
  }

  Process::Inlets& inlets() noexcept { return m_inlets; }
  Process::Outlets& outlets() noexcept { return m_outlets; }
  const Process::Inlets& inlets() const noexcept { return m_inlets; }
  const Process::Outlets& outlets() const noexcept { return m_outlets; }

  void scriptChanged(const QString& str) W_SIGNAL(scriptChanged, str);
  void programChanged() W_SIGNAL(programChanged);
  void errorMessage(int line, const QString& e) W_SIGNAL(errorMessage, line, e);

  PROPERTY(QString, script READ script WRITE setScript NOTIFY scriptChanged)

private:
  QString effect() const noexcept override;
  void loadPreset(const Process::Preset& preset) override;
  Process::Preset savePreset() const noexcept override;

  void init();
  [[nodiscard]] Process::ScriptChangeResult reload();

  QString m_script;
  Lavfi::Description m_desc;
  std::vector<Lavfi::OptionInfo> m_controls;
};

using ProcessFactory = Process::EffectProcessFactory_T<Lavfi::Model>;
}

namespace Process
{
template <>
QString EffectProcessFactory_T<Lavfi::Model>::customConstructionData() const noexcept;

template <>
Process::Descriptor
EffectProcessFactory_T<Lavfi::Model>::descriptor(QString d) const noexcept;

template <>
Process::Descriptor EffectProcessFactory_T<Lavfi::Model>::descriptor(
    const Process::ProcessModel& d) const noexcept;
}
