#pragma once
#include <Process/ProcessMetadata.hpp>

namespace Lavfi
{
class Model;
}

PROCESS_METADATA(
    , Lavfi::Model, "3f0a1b6e-6a6b-4d0c-9d8f-2b3c1e7a9f10",
    "Lavfi",                              // Internal name
    "FFmpeg filter",                      // Pretty name
    Process::ProcessCategory::Script,     // Category
    "Script",                             // Category
    "Runs an FFmpeg libavfilter graph on audio or video. Its ports are the "
    "graph's open pads and the runtime-changeable options of its filters.", // Description
    "ossia team",                                                             // Author
    (QStringList{"ffmpeg", "lavfi", "filter", "video", "audio", "gpu"}),      // Tags
    {},                                                                       // Inputs
    {},                                                                       // Outputs
    QUrl{"https://ffmpeg.org/ffmpeg-filters.html"},                           // Doc link
    Process::ProcessFlags::ExternalEffect | Process::ProcessFlags::DynamicPorts
        | Process::ProcessFlags::ScriptEditingSupported)
DESCRIPTION_METADATA(, Lavfi::Model, "FFmpeg filter")
