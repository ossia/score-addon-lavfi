#pragma once
#include <Process/Drop/ProcessDropHandler.hpp>

#include <Library/LibraryInterface.hpp>
#include <Library/LibrarySettings.hpp>
#include <Library/ProcessesItemModel.hpp>

#include <score/tools/File.hpp>

#include <QFile>

#include <Lavfi/Process.hpp>

namespace Lavfi
{
/**
 * @brief `.lavfi` files in the user library become presets of the process.
 *
 * A .lavfi file is a filtergraph string (comments with '#' at line start are
 * stripped, lines are joined). The repository ships a `presets/` folder to
 * copy into the library; the process is created with the file's content as
 * its program, so the preset does not depend on the file afterwards.
 */
class LibraryHandler final
    : public QObject
    , public Library::LibraryInterface
{
  SCORE_CONCRETE("6a5b0f7e-2c41-4f7d-9d4a-3e8f1b2c7d90")

  QSet<QString> acceptedFiles() const noexcept override { return {"lavfi"}; }

  Library::CategoryPaths categories;

public:
  void setup(Library::ProcessesItemModel& model, const score::GUIApplicationContext& ctx)
      override
  {
    categories.init(Metadata<PrettyName_k, Lavfi::Model>::get().toStdString(), ctx);
  }

  static QString readGraph(const QString& path)
  {
    QFile f{path};
    if(!f.open(QIODevice::ReadOnly))
      return {};
    // One graph per file, possibly over several lines: lines are chained as
    // filters unless the previous one already ends the chain element.
    QString graph;
    for(const auto& raw : QString::fromUtf8(f.readAll()).split('\n'))
    {
      const QString line = raw.trimmed();
      if(line.isEmpty() || line.startsWith('#'))
        continue;
      if(!graph.isEmpty() && !graph.endsWith(',') && !graph.endsWith(';')
         && !graph.endsWith('[') && !line.startsWith(',') && !line.startsWith(';')
         && !line.startsWith('['))
        graph += ',';
      graph += line;
    }
    return graph;
  }

  std::optional<Library::ProcessEntry> scanPath(std::string_view path) override
  {
    score::PathInfo file{path};
    Library::ProcessData pdata;
    pdata.prettyName
        = QString::fromUtf8(file.completeBaseName.data(), file.completeBaseName.size());
    pdata.key = Metadata<ConcreteKey_k, Lavfi::Model>::get();
    pdata.customData = readGraph(
        QString::fromUtf8(file.absoluteFilePath.data(), file.absoluteFilePath.size()));
    if(pdata.customData.isEmpty())
      return std::nullopt;
    return Library::ProcessEntry{pdata.key, categories(file), {std::move(pdata), {}}};
  }
};

class DropHandler final : public Process::ProcessDropHandler
{
  SCORE_CONCRETE("0c9e4d21-7b3a-4e6f-8a15-6d2f9c0b1e57")

  QSet<QString> fileExtensions() const noexcept override { return {"lavfi"}; }

  void dropPath(
      std::vector<ProcessDrop>& vec, const score::FilePath& filename,
      const score::DocumentContext& ctx) const noexcept override
  {
    Process::ProcessDropHandler::ProcessDrop p;
    p.creation.key = Metadata<ConcreteKey_k, Lavfi::Model>::get();
    p.creation.prettyName = filename.basename;
    p.creation.customData = LibraryHandler::readGraph(filename.absolute);
    if(p.creation.customData.isEmpty())
      return;
    vec.push_back(std::move(p));
  }
};
}
