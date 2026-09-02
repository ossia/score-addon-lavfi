#include "Process.hpp"

#include <Process/Dataflow/Port.hpp>
#include <Process/Dataflow/PortFactory.hpp>
#include <Process/Dataflow/WidgetInlets.hpp>
#include <Process/PresetHelpers.hpp>

#include <score/application/ApplicationComponents.hpp>
#include <score/tools/DeleteAll.hpp>
#include <score/tools/IdentifierGeneration.hpp>

#include <Gfx/TexturePort.hpp>

#include <cmath>

#include <wobjectimpl.h>

W_OBJECT_IMPL(Lavfi::Model)

namespace Process
{
template <>
QString EffectProcessFactory_T<Lavfi::Model>::customConstructionData() const noexcept
{
  return "anull";
}

template <>
Process::Descriptor
EffectProcessFactory_T<Lavfi::Model>::descriptor(QString txt) const noexcept
{
  Process::Descriptor d;
  d.prettyName = "FFmpeg filter";
  d.category = Process::ProcessCategory::Script;
  d.categoryText = "Script";
  d.author = "ossia team";
  d.description = txt;
  d.documentationLink = QUrl{"https://ffmpeg.org/ffmpeg-filters.html"};
  d.tags = QStringList{"ffmpeg", "lavfi"};
  return d;
}

template <>
Process::Descriptor EffectProcessFactory_T<Lavfi::Model>::descriptor(
    const Process::ProcessModel& d) const noexcept
{
  return descriptor(static_cast<const Lavfi::Model&>(d).script());
}
}

namespace Lavfi
{
static const QString defaultGraph = QStringLiteral("anull");

Model::Model(
    const TimeVal& duration, const QString& graph, const Id<Process::ProcessModel>& id,
    QObject* parent)
    : Process::ProcessModel{duration, id, "lavfi", parent}
{
  metadata().setInstanceName(*this);
  auto res = setScript(graph.isEmpty() ? defaultGraph : graph);
  (void)res; // Freshly constructed: nothing to delete later.
}

Model::~Model() { }

void Model::init()
{
  // Deserialised: the ports come from the document; only the description
  // (used by the executor to know pad types and options) needs rebuilding.
  std::string err;
  if(!Lavfi::Graph::describe(m_script.toStdString(), m_desc, err))
  {
    // This FFmpeg cannot build the document's graph (a filter it lacks).
    // Keep the ports as saved, so cables survive, and remember what kind of
    // node they need; the graph itself will fail to configure, visibly.
    m_desc = {};
    qDebug() << "lavfi: cannot describe the saved graph:" << QString::fromStdString(err);
  }
  m_controls.clear();
  for(const auto& o : m_desc.options)
    if(o.runtime)
      m_controls.push_back(o);
  m_portsHaveVideo = false;
  for(auto* p : m_inlets)
    if(qobject_cast<Gfx::TextureInlet*>(p))
      m_portsHaveVideo = true;
  for(auto* p : m_outlets)
    if(qobject_cast<Gfx::TextureOutlet*>(p))
      m_portsHaveVideo = true;
}

bool Model::hasVideo() const noexcept
{
  return m_desc.hasVideo() || (m_desc.inputs.empty() && m_desc.outputs.empty() && m_portsHaveVideo);
}

QString Model::prettyName() const noexcept
{
  if(m_desc.filters.empty())
    return tr("FFmpeg filter");
  // "gblur, hflip" reads better than the raw instance names Parsed_gblur_0.
  QString s;
  for(const auto& f : m_desc.filters)
  {
    QString name = QString::fromStdString(f);
    if(name.startsWith("Parsed_"))
    {
      name.remove(0, 7);
      if(auto i = name.lastIndexOf('_'); i > 0)
        name.truncate(i);
    }
    if(!s.isEmpty())
      s += ", ";
    s += name;
    if(s.size() > 40)
    {
      s += "…";
      break;
    }
  }
  return s;
}

QString Model::effect() const noexcept
{
  return m_script;
}

bool Model::validate(const QString& txt) const noexcept
{
  Lavfi::Description desc;
  std::string err;
  if(!Lavfi::Graph::describe(txt.toStdString(), desc, err))
  {
    const_cast<Model&>(*this).errorMessage(0, QString::fromStdString(err));
    return false;
  }
  return true;
}

Process::ScriptChangeResult Model::setScript(const QString& txt)
{
  if(txt == m_script)
    return {};
  m_script = txt;
  if(m_script.isEmpty())
    m_script = defaultGraph;
  auto res = reload();
  scriptChanged(m_script);
  return res;
}

namespace
{
// Ids must stay unique across the ports we keep and the ones we create.
struct IdGen
{
  int next{};
  template <typename Container>
  explicit IdGen(const Container& ports)
  {
    for(auto* p : ports)
      next = std::max(next, p->id().val() + 1);
  }
  Id<Process::Port> operator()() { return Id<Process::Port>{next++}; }
};

template <typename Port, typename Container>
Port* takeExisting(Container& old, const QString& name)
{
  for(auto it = old.begin(); it != old.end(); ++it)
  {
    if(auto p = dynamic_cast<Port*>(*it); p && p->name() == name)
    {
      old.erase(it);
      return p;
    }
  }
  return nullptr;
}

bool finiteRange(double min, double max, double limit)
{
  return std::isfinite(min) && std::isfinite(max) && max > min && std::abs(min) <= limit
         && std::abs(max) <= limit;
}

Process::Inlet* makeControl(
    const Lavfi::OptionInfo& o, const QString& name, Id<Process::Port> id, QObject* parent)
{
  switch(o.type)
  {
    case AV_OPT_TYPE_BOOL:
      return new Process::Toggle{o.def != 0., name, id, parent};

    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_INT64:
    case AV_OPT_TYPE_UINT64:
    case AV_OPT_TYPE_FLAGS: {
      if(!o.consts.empty())
      {
        // Named values are sent back by name: avfilter_process_command
        // accepts them, and they read better in the UI and in saved files.
        std::vector<std::pair<QString, ossia::value>> values;
        ossia::value init;
        for(const auto& c : o.consts)
        {
          QString n = QString::fromStdString(c.name);
          values.emplace_back(n, n.toStdString());
          if(c.value == o.def)
            init = n.toStdString();
        }
        if(!values.empty() && !init.valid())
          init = values.front().second;
        return new Process::ComboBox{std::move(values), std::move(init), name, id, parent};
      }
      const int min = int(std::max(o.min, -1e9)), max = int(std::min(o.max, 1e9));
      const int def = int(std::clamp(o.def, double(min), double(max)));
      if(finiteRange(o.min, o.max, 100000.))
        return new Process::IntSlider{min, max, def, name, id, parent};
      return new Process::IntSpinBox{min, max, def, name, id, parent};
    }

    case AV_OPT_TYPE_FLOAT:
    case AV_OPT_TYPE_DOUBLE: {
      const float min = float(std::max(o.min, -1e9)), max = float(std::min(o.max, 1e9));
      const float def = std::clamp(float(o.def), min, max);
      if(finiteRange(o.min, o.max, 1e6))
        return new Process::FloatSlider{min, max, def, name, id, parent};
      return new Process::FloatSpinBox{min, max, def, name, id, parent};
    }

    default:
      // string, color, duration, rational, video size/rate, pixel format...:
      // lavfi parses all of them from text.
      return new Process::LineEdit{QString::fromStdString(o.def_str), name, id, parent};
  }
}
}

Process::ScriptChangeResult Model::reload()
{
  Process::ScriptChangeResult res;

  Lavfi::Description desc;
  std::string err;
  if(!Lavfi::Graph::describe(m_script.toStdString(), desc, err))
  {
    errorMessage(0, QString::fromStdString(err));
    res.valid = false;
    return res;
  }
  m_desc = std::move(desc);
  m_controls.clear();

  Process::Inlets old_in = m_inlets;
  Process::Outlets old_out = m_outlets;
  Process::Inlets new_in;
  Process::Outlets new_out;
  IdGen in_id{m_inlets};
  IdGen out_id{m_outlets};

  // --- pads --------------------------------------------------------------------
  for(const auto& pad : m_desc.inputs)
  {
    const QString name = QString::fromStdString(pad.name);
    if(pad.type == AVMEDIA_TYPE_AUDIO)
    {
      if(auto p = takeExisting<Process::AudioInlet>(old_in, name))
        new_in.push_back(p);
      else
        new_in.push_back(new Process::AudioInlet{name, in_id(), this});
    }
    else if(pad.type == AVMEDIA_TYPE_VIDEO)
    {
      if(auto p = takeExisting<Gfx::TextureInlet>(old_in, name))
        new_in.push_back(p);
      else
        new_in.push_back(new Gfx::TextureInlet{name, in_id(), this});
    }
  }
  for(const auto& pad : m_desc.outputs)
  {
    const QString name = QString::fromStdString(pad.name);
    if(pad.type == AVMEDIA_TYPE_AUDIO)
    {
      if(auto p = takeExisting<Process::AudioOutlet>(old_out, name))
        new_out.push_back(p);
      else
      {
        auto out = new Process::AudioOutlet{name, out_id(), this};
        if(new_out.empty())
          out->setPropagate(true);
        new_out.push_back(out);
      }
    }
    else if(pad.type == AVMEDIA_TYPE_VIDEO)
    {
      if(auto p = takeExisting<Gfx::TextureOutlet>(old_out, name))
        new_out.push_back(p);
      else
        new_out.push_back(new Gfx::TextureOutlet{name, out_id(), this});
    }
  }

  // --- runtime options -> control inlets -----------------------------------------
  for(const auto& o : m_desc.options)
  {
    if(!o.runtime)
      continue;
    const QString name = QString::fromStdString(o.portName());
    Process::Inlet* kept = nullptr;
    // Keep a control if it has the same name and the same widget kind: the
    // kind is a function of the option type, so a same-named option of the
    // same filter keeps its value and its cables.
    for(auto it = old_in.begin(); it != old_in.end(); ++it)
    {
      auto ctl = dynamic_cast<Process::ControlInlet*>(*it);
      if(!ctl || ctl->name() != name)
        continue;
      std::unique_ptr<Process::Inlet> probe{makeControl(o, name, Id<Process::Port>{-1}, nullptr)};
      const Process::Inlet& probed = *probe;
      const Process::Inlet& existing = *ctl;
      if(typeid(probed) == typeid(existing))
      {
        kept = ctl;
        old_in.erase(it);
      }
      break;
    }
    new_in.push_back(kept ? kept : makeControl(o, name, in_id(), this));
    m_controls.push_back(o);
  }

  // --- metadata outlet --------------------------------------------------------------
  {
    const QString name = QStringLiteral("Metadata");
    if(auto p = takeExisting<Process::ValueOutlet>(old_out, name))
      new_out.push_back(p);
    else
      new_out.push_back(new Process::ValueOutlet{name, out_id(), this});
  }

  // Whatever was not reused goes away, after the command had a chance to
  // save its cables.
  for(auto* p : old_in)
  {
    p->setParent(nullptr);
    res.inlets.container.push_back(p);
  }
  for(auto* p : old_out)
  {
    p->setParent(nullptr);
    res.outlets.container.push_back(p);
  }
  m_inlets = std::move(new_in);
  m_outlets = std::move(new_out);
  m_portsHaveVideo = m_desc.hasVideo();

  // programChanged is the command's to emit (Scenario::EditScript does, once
  // the ports and cables are settled), not reload's.
  res.valid = true;
  return res;
}

void Model::loadPreset(const Process::Preset& preset)
{
  // The graph string travels in preset.key.effect and is applied through
  // the script command (cables of dropped ports saved); only the control
  // values are here. Same as the Faust and JS processes.
  Process::loadScriptProcessPreset<Model::p_script>(*this, preset);
}

Process::Preset Model::savePreset() const noexcept
{
  return Process::saveScriptProcessPreset(*this, m_script);
}
}

template <>
void DataStreamReader::read(const Lavfi::Model& proc)
{
  m_stream << proc.m_script;
  readPorts(*this, proc.m_inlets, proc.m_outlets);
  insertDelimiter();
}

template <>
void DataStreamWriter::write(Lavfi::Model& proc)
{
  m_stream >> proc.m_script;
  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);
  checkDelimiter();
}

template <>
void JSONReader::read(const Lavfi::Model& proc)
{
  obj["Script"] = proc.m_script;
  readPorts(*this, proc.m_inlets, proc.m_outlets);
}

template <>
void JSONWriter::write(Lavfi::Model& proc)
{
  proc.m_script = obj["Script"].toString();
  writePorts(
      *this, components.interfaces<Process::PortFactoryList>(), proc.m_inlets,
      proc.m_outlets, &proc);
}
